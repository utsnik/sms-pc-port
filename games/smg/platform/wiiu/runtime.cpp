#include "runtime.h"

#include "dol_data.h"

#include "port/heap_routing.h"
#include "port/port.h"

#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/proc.h>

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <math.h>
#include <cassert>
#include <stdarg.h>
#include <stdio.h>

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
size_t s_mem2_arena_used;
int s_initialized;
int s_alloc_scope_depth;
int s_log_file_warned;
int s_sleep_warned;
int s_file_warned;
int s_input_warned;
int s_audio_warned;
unsigned s_app_root_index;

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

void* arena_alloc(size_t size, size_t alignment)
{
    if (alignment < 4 || (alignment & (alignment - 1)) != 0)
        alignment = 4;

    const size_t aligned = (s_mem2_arena_used + alignment - 1) & ~(alignment - 1);
    if (aligned > kMem2ArenaBytes || size > kMem2ArenaBytes - aligned)
        return nullptr;

    void* result = s_mem2_arena + aligned;
    s_mem2_arena_used = aligned + size;
    return result;
}

} // namespace

extern "C" {

/* Sunshine's shared GX parser uses these names for its 32-bit target window.
 * Stage 1 points that window at fixed target-owned storage. */
u8* port_mem1_base = s_mem1_arena;
u32 port_mem1_size = (u32)kMem1ArenaBytes;
u8* sms_wiiu_mem2_base = s_mem2_arena;
u32 sms_wiiu_mem2_size = (u32)kMem2ArenaBytes;

void* smg_wiiu_arena_alloc(size_t size, size_t alignment)
{
    return arena_alloc(size, alignment);
}

uint32_t smg_wiiu_arena_used(void)
{
    return (uint32_t)s_mem2_arena_used;
}

const char* sms_wiiu_app_root(void) { return kAppRootCandidates[s_app_root_index]; }
uint32_t sms_wiiu_heap_free(void) { return 0; }

void smg_wiiu_runtime_init(void)
{
    if (s_initialized)
        return;
    assert((reinterpret_cast<uintptr_t>(s_mem1_arena) & 63u) == 0);
    assert((reinterpret_cast<uintptr_t>(s_mem2_arena) & 63u) == 0);
    resolve_app_root();
    s_initialized = 1;
    port_log("SMG Wii U: arenas MEM1=25165824 MEM2=67108864 alignment=64");
    port_log("SMG Wii U: stage2 runtime initialized; static arenas");
    smg_wiiu_load_dol_data();
    smg_wiiu_gx_link_stub_init();
}

void port_log(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    WHBLogPrintf("%s", line);
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
    WHBProcStopRunning();
    for (;;) {
    }
}

void port_log_file(const char*)
{
    log_unsupported_once(&s_log_file_warned, "port_log_file");
}

void* port_host_alloc(size_t size, int alignment)
{
    return arena_alloc(size, alignment > 0 ? (size_t)alignment : 4u);
}

void port_host_free(void*)
{
    /* The fixed arena is reclaimed with the process. */
}

int port_use_game_heap(void) { return 0; }

int port_is_game_heap_ptr(const void* pointer)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(s_mem2_arena);
    return p >= lo && p < lo + kMem2ArenaBytes;
}

void port_host_alloc_scope_enter(void) { ++s_alloc_scope_depth; }

void port_host_alloc_scope_leave(void)
{
    if (s_alloc_scope_depth > 0)
        --s_alloc_scope_depth;
}

void* port_low_alloc(size_t size) { return arena_alloc(size, 64); }
int port_mem_init(void) { return 1; }
void port_mem_set_disc_id(const char*) {}
int port_is_game_memory(const void* p) { return port_is_game_heap_ptr(p); }

void* port_phys_to_host(uint32_t physical)
{
    return physical < kMem2ArenaBytes ? s_mem2_arena + physical : nullptr;
}

uint32_t port_host_to_phys(const void* pointer)
{
    if (!port_is_game_memory(pointer))
        return 0;
    return (uint32_t)(reinterpret_cast<const unsigned char*>(pointer) - s_mem2_arena);
}

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

/* The runtime has no SDL, file, or foreground audio service in Stage 1. */
int sms_wiiu_sdl_audio_open(int) { return 0; }
void sms_wiiu_sdl_audio_queue(const void*, uint32_t) {}
uint32_t sms_wiiu_sdl_audio_queued(void) { return 0; }
void sms_wiiu_sdl_audio_close(void) {}
void sms_wiiu_ax_load(u32*, u32*) {}
void sms_wiiu_hang_check(void) {}
volatile int sms_wiiu_crash_stage;
volatile int sms_wiiu_crash_pending;
void sms_wiiu_crash_return_to_menu(void) { WHBProcStopRunning(); }
void sms_wiiu_profiler_tick(void) {}
void sms_wiiu_profiler_stop(void) {}
int sms_wiiu_gfx_is_initialized(void) { return 0; }
void sms_wiiu_gfx_frame(void) {}
uint32_t sms_wiiu_presents(void) { return 0; }
uint32_t sms_wiiu_vsync_fields(void) { return 0; }
void sms_wiiu_exit(void) { WHBProcStopRunning(); }

} // extern "C"
