#include "runtime.h"

#include "dol_data.h"
#include "host_alloc.h"

#include "port/heap_routing.h"
#include "port/port.h"
#include "wiiu/smg_arena.h"

#include <coreinit/fastmutex.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/proc.h>

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <math.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <malloc.h>
#include <stdarg.h>

namespace JASystem { namespace AudioThread {
alignas(32) unsigned char jac_audioThread[0x200];
} }

extern "C" u32 __OSFpscrEnableBits;
u32 __OSFpscrEnableBits;

namespace {

constexpr size_t kMem1ArenaBytes = 24u * 1024u * 1024u;
constexpr size_t kMem2ArenaBytes = 64u * 1024u * 1024u;
static_assert(kMem1ArenaBytes == 25165824u, "SMG MEM1 arena size changed");
static_assert(kMem2ArenaBytes == 67108864u, "SMG MEM2 arena size changed");
const char kAppRootCandidates[][128] = {
    "fs:/vol/external01/wiiu/apps/smg/",
    "wiiu/apps/smg/",
    "/vol/external01/wiiu/apps/smg/",
};

alignas(64) unsigned char s_mem1_arena[kMem1ArenaBytes];
alignas(64) unsigned char s_mem2_arena[kMem2ArenaBytes];
uint32_t s_varam_base;
uint32_t s_varam_size;
int s_varam_overhang_logged;
int s_initialized;
int s_game_heap_ready;
int s_log_file_warned;
int s_sleep_warned;
int s_file_warned;
int s_input_warned;
int s_audio_warned;
unsigned s_app_root_index;

/* Game, audio and loader threads all log: without a lock their open/append/
 * close sequences interleaved and cut each other's lines. */
OSFastMutex s_log_mutex;
bool s_log_mutex_ready;

void write_stage2_log_locked(const char* line);

void write_stage2_log(const char* line)
{
    if (!s_log_mutex_ready) {
        write_stage2_log_locked(line);
        return;
    }
    OSFastMutex_Lock(&s_log_mutex);
    write_stage2_log_locked(line);
    OSFastMutex_Unlock(&s_log_mutex);
}

void write_stage2_log_locked(const char* line)
{
    char marker_path[192];
    std::snprintf(marker_path, sizeof(marker_path), "%sstage2log",
                  kAppRootCandidates[s_app_root_index]);
    FILE* marker = std::fopen(marker_path, "rb");
    if (!marker)
        return;
    std::fclose(marker);

    char log_path[192];
    std::snprintf(log_path, sizeof(log_path), "%sstage2.log",
                  kAppRootCandidates[s_app_root_index]);
    FILE* log = std::fopen(log_path, "ab");
    if (!log)
        return;
    const size_t length = std::strlen(line);
    std::fwrite(line, 1, length, log);
    if (length == 0 || line[length - 1] != '\n')
        std::fputc('\n', log);
    std::fflush(log);
    std::fclose(log);
}

void resolve_app_root()
{
    for (unsigned i = 0; i < sizeof(kAppRootCandidates) / sizeof(kAppRootCandidates[0]); ++i) {
        char probe[160];
        std::snprintf(probe, sizeof(probe), "%ssys/fst.bin", kAppRootCandidates[i]);
        FILE* file = std::fopen(probe, "rb");
        port_log("SMG Wii U: app root probe %s -> %s", kAppRootCandidates[i],
                 file ? "ok" : "missing");
        if (file) {
            std::fclose(file);
            s_app_root_index = i;
            return;
        }
    }
    port_log("SMG Wii U: no app root holds sys/fst.bin; keeping %s",
             kAppRootCandidates[s_app_root_index]);
}

void log_unsupported_once(int* flag, const char* what)
{
    if (*flag)
        return;
    *flag = 1;
    port_log("SMG Wii U: unsupported Stage 1 call: %s", what);
}

} // namespace

extern "C" {

/* Sunshine's shared GX parser uses these names for its 32-bit target window.
 * Stage 1 points that window at fixed target-owned storage. */
u8* port_mem1_base = s_mem1_arena;
u32 port_mem1_size = (u32)kMem1ArenaBytes;
u8* sms_wiiu_mem2_base = s_mem2_arena;
u32 sms_wiiu_mem2_size = (u32)kMem2ArenaBytes;

/* smg-audio: D3. The DSP HLE's ARAM reads (platform/audio/dsp_hle.cpp): the
 * DsetVARAM base plus an ARAM offset is a MEM2 address, inside the window
 * aralt's ARInit maps (the MEM2 arena low mark createRootHeap reserved).
 * Ruling R18: a read past the 0xE00000 window would alias the GDDR3 heap
 * header that follows it, so it resolves to nothing and is logged once. */
const uint8_t* port_varam_ptr(uint32_t addr)
{
    bool beyond = false;
    const uint8_t* p = smg_wiiu::varam_lookup(s_varam_base, s_varam_size, addr, &beyond);
    if (beyond && !s_varam_overhang_logged) {
        s_varam_overhang_logged = 1;
        port_log("[smg-audio] VARAM read beyond the 0x%08x window dropped: offset=0x%08x",
                 smg_wiiu::kAramWindowBytes, addr - s_varam_base);
    }
    return p;
}

void smg_wiiu_bind_varam_base(uint32_t base, uint32_t size)
{
    const uint32_t arena_lo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sms_wiiu_mem2_base));
    const uint32_t arena_hi = arena_lo + sms_wiiu_mem2_size;
    if (base < arena_lo || base > arena_hi || size > arena_hi - base ||
        size < smg_wiiu::kAramBaseOffset ||
        size > smg_wiiu::kAramWindowBytes) {
        port_log("[smg-audio] invalid ARAM base=0x%08x for MEM2 %p..%p",
                 base, sms_wiiu_mem2_base, sms_wiiu_mem2_base + sms_wiiu_mem2_size);
        s_varam_base = 0;
        return;
    }
    s_varam_base = base;
    s_varam_size = size;
}

const char* sms_wiiu_app_root(void) { return kAppRootCandidates[s_app_root_index]; }
int sms_wiiu_sd_marker(const char* name)
{
    char path[192];
    std::snprintf(path, sizeof(path), "%s%s", sms_wiiu_app_root(), name);
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return 0;
    std::fclose(file);
    return 1;
}
const char* smg_wiiu_save_root(void)
{
    static char save_root[160];
    std::snprintf(save_root, sizeof(save_root), "%ssave",
                  kAppRootCandidates[s_app_root_index]);
    return save_root;
}
uint32_t sms_wiiu_heap_free(void) { return 0; }

void smg_wiiu_runtime_init(void)
{
    if (s_initialized)
        return;
    assert((reinterpret_cast<uintptr_t>(s_mem1_arena) & 63u) == 0);
    assert((reinterpret_cast<uintptr_t>(s_mem2_arena) & 63u) == 0);
    OSFastMutex_Init(&s_log_mutex, "smg-stage2-log");
    s_log_mutex_ready = true;
    resolve_app_root();
    s_initialized = 1;
    port_log("SMG Wii U: arenas MEM1=25165824 MEM2=67108864 alignment=64");
    port_log("SMG Wii U: app root=%s", sms_wiiu_app_root());
    port_log("SMG Wii U: stage2 runtime initialized; static arenas");
}

/* Wii system language (SC_LANG_*). The Stage 1 stub returned 0, which is
 * SC_LANG_JAPANESE: MR::getDecidedLanguageFromIPL() then picked JpJapanese on
 * the PAL disc (row RMGP), whose data has only the Eu* language trees, so every
 * language-dependent path (makeFileNameConsideringLanguage) was missing.
 * English is the PAL default; mapping the Wii U system language is later work. */
u8 SCGetLanguage(void) { return 1; /* SC_LANG_ENGLISH */ }

void port_log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    WHBLogPrintf("%s", line);
    write_stage2_log(line);
}

void port_fatal(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    WHBLogPrintf("SMG Wii U fatal: %s", line);
    /* The spin below shows nothing in Cemu: put the reason in stage2.log too. */
    char fatal_line[1040];
    std::snprintf(fatal_line, sizeof(fatal_line), "SMG Wii U fatal: %s", line);
    write_stage2_log(fatal_line);
    WHBProcStopRunning();
    for (;;) {
    }
}

void port_log_file(const char*)
{
    log_unsupported_once(&s_log_file_warned, "port_log_file");
}

/* Port-internal allocations (Galaxy's operator new before the JKR root heap
 * exists, the DSP HLE's async Command/Reply): wut's default heap, outside
 * both Galaxy arenas, freed with free(). */
void* port_host_alloc(size_t size, int alignment)
{
    return memalign(smg_wiiu::host_alloc_alignment(alignment), size);
}

void port_host_free(void* pointer)
{
    std::free(pointer);
}

void smg_wiiu_game_heap_ready(void) { s_game_heap_ready = 1; }

/* Port code reaches the system heap through SMS_PORT_HEAP_SCOPE and the
 * platform/wiiu/port_heap.cpp operator new wraps (ruling R17), before
 * Galaxy's operator new is consulted. */
int port_use_game_heap(void) { return s_game_heap_ready; }

int port_is_game_heap_ptr(const void* pointer)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t mem1_lo = reinterpret_cast<uintptr_t>(s_mem1_arena);
    const uintptr_t mem2_lo = reinterpret_cast<uintptr_t>(s_mem2_arena);
    return (p >= mem1_lo && p < mem1_lo + kMem1ArenaBytes) ||
           (p >= mem2_lo && p < mem2_lo + kMem2ArenaBytes);
}

void* port_low_alloc(size_t size) { return memalign(64, size); }
int port_mem_init(void) { return 1; }
void port_mem_set_disc_id(const char*) {}
int port_is_game_memory(const void* p) { return port_is_game_heap_ptr(p); }

void port_irq_set_handler(int, PortIrqHandler) {}
void port_irq_raise(int) {}
void port_irq_poll(void) {}

int64_t port_host_time_ns(void)
{
    const OSTime ticks = OSGetTime();
    return (int64_t)(((uint64_t)ticks * 1000000000ull) / 60750000ull);
}

void port_host_sleep_ns(int64_t)
{
    log_unsupported_once(&s_sleep_warned, "port_host_sleep_ns");
}

void port_perf_frame_begin(void) {}
void port_perf_frame_work_done(void) {}
void port_perf_record(int64_t) {}
void port_perf_prepare(int64_t) {}
void port_perf_render(int64_t, int64_t) {}
void port_perf_gpu_wait(int64_t) {}

void port_input_use_tilt(float)
{
    log_unsupported_once(&s_input_warned, "input");
}
void port_input_request_pause(void)
{
    log_unsupported_once(&s_input_warned, "input");
}
int port_input_take_pause_request(void) { return 0; }
void port_input_discard_pause_request(void) {}
int port_input_real_a(void) { return 0; }
int port_input_camera_inverted(void) { return 0; }
void port_input_set_camera_inverted(int)
{
    log_unsupported_once(&s_input_warned, "input");
}

int port_vr_cull_view(float*, float*, float*) { return 0; }
int port_vr_move_axes(float*, float*, float*) { return 0; }
int port_vr_pointer_in_world(void) { return 0; }
int port_vr_pointer_ray(float*, float*) { return 0; }
void port_vr_pointer_reach(float) {}
void port_vr_pointer_depth(float) {}
void port_vr_pointer_touched(unsigned long long) {}
void port_vr_wipe(int, float, unsigned int) {}
int port_vr_diorama(void) { return 0; }
void port_vr_transit_begin(const void*) {}
void port_vr_transit_end(const void*) {}
const void* port_vr_transit_host(void) { return nullptr; }
void port_vr_pause_menu(int) {}

void port_skip_context(int) {}
int port_skip_take(int) { return 0; }
int port_skip_fast_forwarding(void) { return 0; }
int port_skip_interrupt(void) { return 0; }
void port_skip_scene_change(void) {}

int port_language_count(void) { return 0; }
const char* port_language_name(int) { return nullptr; }
void port_language_set(const char*) {}
int port_language_wanted(void) { return 0; }
int port_language_running(void) { return -1; }
int port_language_start(void) { return 0; }

const PortDisc* port_dvd_identify(const char*) { return nullptr; }
const PortDisc* port_dvd_disc(void) { return nullptr; }

void port_speaker_push(const s16*, int)
{
    log_unsupported_once(&s_audio_warned, "speaker audio");
}

void* port_file_open(const char*, size_t*)
{
    log_unsupported_once(&s_file_warned, "file access");
    return nullptr;
}
int port_file_read(void*, void*, size_t)
{
    log_unsupported_once(&s_file_warned, "file access");
    return 0;
}
void port_file_close(void*) {}

/* The Stage 1 link keeps the game-facing MTX entry points available while
 * deferring the renderer. These are small ABI-complete fallbacks, not a new
 * platform provider. */
void PSMTXMultVecArraySR(const Mtx m, const Vec* src, Vec* dst, u32 count)
{
    for (u32 i = 0; i < count; ++i)
        PSMTXMultVecSR(m, &src[i], &dst[i]);
}

void PSMTX44Identity(Mtx44 m)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[r][c] = r == c ? 1.0f : 0.0f;
}

void PSMTX44Copy(const Mtx44 src, Mtx44 dst)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            dst[r][c] = src[r][c];
}

f32 PSQUATDotProduct(const Quaternion* a, const Quaternion* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
}

void PSQUATMultiply(const Quaternion* a, const Quaternion* b, Quaternion* out)
{
    Quaternion value = {
        a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y,
        a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x,
        a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w,
        a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z};
    *out = value;
}

void C_QUATMtx(Quaternion* out, const Mtx m)
{
    const f32 trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const f32 s = sqrtf(trace + 1.0f) * 2.0f;
        out->w = 0.25f * s;
        out->x = (m[2][1] - m[1][2]) / s;
        out->y = (m[0][2] - m[2][0]) / s;
        out->z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const f32 s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        out->w = (m[2][1] - m[1][2]) / s;
        out->x = 0.25f * s;
        out->y = (m[0][1] + m[1][0]) / s;
        out->z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const f32 s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        out->w = (m[0][2] - m[2][0]) / s;
        out->x = (m[0][1] + m[1][0]) / s;
        out->y = 0.25f * s;
        out->z = (m[1][2] + m[2][1]) / s;
    } else {
        const f32 s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        out->w = (m[1][0] - m[0][1]) / s;
        out->x = (m[0][2] + m[2][0]) / s;
        out->y = (m[1][2] + m[2][1]) / s;
        out->z = 0.25f * s;
    }
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* out, f32 t)
{
    Quaternion b = *q;
    f32 cosine = PSQUATDotProduct(p, &b);
    if (cosine < 0.0f) {
        cosine = -cosine;
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
    }
    if (cosine > 0.9995f) {
        out->x = p->x + t * (b.x - p->x);
        out->y = p->y + t * (b.y - p->y);
        out->z = p->z + t * (b.z - p->z);
        out->w = p->w + t * (b.w - p->w);
        return;
    }
    const f32 angle = acosf(cosine);
    const f32 inv = 1.0f / sinf(angle);
    const f32 a = sinf((1.0f - t) * angle) * inv;
    const f32 c = sinf(t * angle) * inv;
    out->x = a * p->x + c * b.x;
    out->y = a * p->y + c * b.y;
    out->z = a * p->z + c * b.z;
    out->w = a * p->w + c * b.w;
}

void smg_wiiu_audio_init(void)
{
    sms_wiiu_sdl_audio_preopen(32000);
}
void sms_wiiu_exit(void) { WHBProcStopRunning(); }

} // extern "C"

void port_irq_defer(std::function<void()> fn)
{
    if (fn)
        fn();
}

void port_irq_run_now(std::function<void()> fn)
{
    if (fn)
        fn();
}
