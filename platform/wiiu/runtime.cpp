#include "runtime.h"

#include "port_stub.h"
#include "port_os.h"
#include <sms_gx/gx_pc.h>

#include <coreinit/memory.h>
#include <coreinit/memblockheap.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <whb/log.h>
#include <whb/proc.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace {

/* "/fs/vol/external01/..." is the FTP server's view and fails silently in the
 * app's stdio. Probe the forms that can work and keep the first that opens
 * sys/fst.bin (wut devoptab first, then cwd-relative for an SD-root cwd). */
/* Keep these as character arrays, rather than an array of pointers.  Cemu 2.6
 * does not apply RPX R_PPC_ADDR32 relocations from writable .data slots to
 * .rodata, even though Cafe's loader does.  A pointer here would therefore
 * be NULL before the first DVD call. */
const char kAppRootCandidates[][128] = {
	"fs:/vol/external01/wiiu/apps/sms/",
	"wiiu/apps/sms/",
	"/vol/external01/wiiu/apps/sms/",
};
unsigned kAppRootIndex;
const uint32_t kMem1Reserve = 0x10000u;
const uint32_t kGameArenaBytes = 96u * 1024u * 1024u;
const uint32_t kLogBufferBytes = 64u * 1024u;

alignas(64) unsigned char s_log_buffer[kLogBufferBytes];
FILE* s_log;
char s_files_root[128];
bool s_initialized;
bool s_heap_ready;
port_stub_rec* s_stub_list;
unsigned s_stub_distinct;

bool pointer_above_32(const void* ptr)
{
#if UINTPTR_MAX > 0xffffffffu
	return reinterpret_cast<uintptr_t>(ptr) > 0xffffffffu;
#else
	(void)ptr;
	return false;
#endif
}

uint32_t default_heap_free()
{
	/* MEM2's base heap is an expanded heap: the block-heap query always said 0. */
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
	return heap ? MEMGetAllocatableSizeForExpHeapEx(heap, 0x40) : 0;
}

void resolve_app_root()
{
	for (unsigned i = 0; i < sizeof(kAppRootCandidates) / sizeof(kAppRootCandidates[0]); ++i) {
		const char* root = kAppRootCandidates[i];
		char probe[160];
		std::snprintf(probe, sizeof(probe), "%ssys/fst.bin", root);
		FILE* file = std::fopen(probe, "rb");
		WHBLogPrintf("SMS Wii U: app root probe %s -> %s", root, file ? "ok" : "missing");
		if (file) {
			std::fclose(file);
			kAppRootIndex = i;
			return;
		}
	}
	WHBLogPrintf("SMS Wii U: no app root holds sys/fst.bin; keeping %s",
	             kAppRootCandidates[kAppRootIndex]);
}

void open_log()
{
	/* No SD log: a console wedge needs a forced power-off, and a file left open
	 * for writing then can damage the card's filesystem (2026-10-17 lost an
	 * Aroma file that way). Logs go over UDP only. */
	return;
	char path[160];
	std::snprintf(path, sizeof(path), "%sstage2.log",
	              kAppRootCandidates[kAppRootIndex]);
	s_log = std::fopen(path, "wb");
	if (s_log) {
		/* Wii U stdio is allowed here, but the buffer must be real storage. */
		std::setvbuf(s_log, reinterpret_cast<char*>(s_log_buffer), _IOFBF,
		             sizeof(s_log_buffer));
	}
}

} // namespace

extern "C" {

u8* port_mem1_base;
u32 port_mem1_size;
u8* sms_wiiu_mem2_base;
u32 sms_wiiu_mem2_size;

const char* port_disc_root;
int port_disc_explicit;
#ifndef SMS_WIIU_SKIP_INTRO
#define SMS_WIIU_SKIP_INTRO 1
#endif
int port_skip_movies = SMS_WIIU_SKIP_INTRO ? 1 : 0;
int port_heat_haze;
int port_pause_tracker;
float port_widescreen = 1.0f;
int port_frame_rate = 30;
int port_active_frame_rate = 30;
int port_high_fps_active;
int port_cheat_god_mode;
int port_cheat_movement_speed = 1;
int port_cheat_jump_height = 1;
int port_cheat_infinite_lives;
int port_cheat_infinite_fludd;
int port_cheat_coin_multiplier = 1;
int port_free_camera;
float port_camera_speed_x = 1.0f;
float port_camera_speed_y = 1.0f;
float port_rumble_boost = 1.0f;
int port_language_index(void) { return 0; }
unsigned char lbl_80004600[4] = {};
int (*port_warp_scenario_name_id)(int, int);

unsigned int sms_mod_generation = 1;
uintptr_t sms_mod_gpr[32] = {};
void* sms_mod_target(unsigned int) { return nullptr; }
int sms_mod_word(unsigned int, unsigned int*) { return 0; }
void sms_mod_bind_data(unsigned int, void*, int) {}
int sms_mod_active(void) { return 0; }
void sms_mod_start(void) {}
void port_cheat_unused(void) {}

void port_savestate_hotkey(int) {}
int port_savestate_scan(void) { return 0; }
const void* port_savestate_scanned(int) { return nullptr; }
const void* port_savestate_picture(int, int, int*, int*) { return nullptr; }
const void* port_savestate_world_art(int, int, int*, int*, int*) { return nullptr; }
int port_savestate_take(char*, int) { return 0; }
void port_savestate_set_ready(int) {}
int port_savestate_save(const void*) { return 0; }
int port_savestate_can_load_exact(const char*) { return 0; }
int port_savestate_load_exact(const char*, void*, int*) { return 0; }
void port_savestate_notify(const char*) {}
void port_savestate_hint(void*) {}

void port_warp_list_begin(void) {}
int port_warp_list_wanted(void) { return 0; }
void port_warp_list_add(const char*, const char*, const char*, int, int) {}
void port_warp_list_end(void) {}
void port_warp_set_ready(int) {}
int port_warp_take(int*, int*) { return 0; }
void port_warp_want_list(void) {}
int port_warp_count(void) { return 0; }
int port_warp_entry(int, void*) { return 0; }
int port_warp_ready(void) { return 0; }
void port_warp_request(int, int) {}

void port_presence_frame(const void*) {}
const void* port_button_glyph(int) { return nullptr; }
int port_draw_hd_thp(uint8_t*, uint8_t*, uint8_t*, int16_t, int16_t,
                     int16_t, int16_t, int16_t, int16_t) { return 0; }
const char* port_hd_cutscene_directory(void) { return nullptr; }
/* __DSP_boot_task comes from platform/audio/dsp_hle.cpp (DSP HLE). */

const char* sms_wiiu_app_root(void) { return kAppRootCandidates[kAppRootIndex]; }

uint32_t sms_wiiu_heap_free(void) { return default_heap_free(); }

int sms_wiiu_sd_marker(const char* name)
{
	char path[256];
	std::snprintf(path, sizeof(path), "%s%s", sms_wiiu_app_root(), name);
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return 0;
	std::fclose(f);
	return 1;
}

void port_log(const char* fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = '\0';
	WHBLogPrintf("%s", line);
	if (s_log) {
		std::fwrite(line, 1, std::strlen(line), s_log);
		std::fflush(s_log);
	}
}

/* TEMPORARY probe (decomp-patches/zzzz-wiiu-92-probe-dabr.patch): arm the
 * data-address breakpoint on writes to the doubleword holding `addr` when the
 * SD marker file `dabr` reads "<tag> <stage>".  A hit is a DSI that the crash
 * handler reports with the writer's PC and backtrace. */
BOOL OSSetDABR(BOOL allCores, uint32_t address, BOOL matchReads, BOOL matchWrites);

static uint32_t s_watch_addr; /* armed doubleword, 0 when none */

/* Software half of the probe: DMA-like copies (DVD reads, ARAM->MRAM) may not
 * trip the DABR, so their destinations are checked against the watch here. */
void port_watch_check(const void* dst, uint32_t len, const char* what, const char* detail, uint32_t extra)
{
	static unsigned s_hits;
	uint32_t d = (uint32_t)(uintptr_t)dst;
	if (!s_watch_addr || s_watch_addr + 8 <= d || d + len <= s_watch_addr)
		return;
	if (s_hits++ < 32)
		port_log("[dabr] COPY HITS WATCH %08X: %s dst=%08X len=0x%x %s extra=0x%x caller=%p\n", (unsigned)s_watch_addr,
		         what, (unsigned)d, (unsigned)len, detail ? detail : "", (unsigned)extra, __builtin_return_address(0));
}

void port_dabr_watch(const void* addr, const char* tag, int stage)
{
	static int s_read = 0;
	static char s_tag[32];
	static char s_mode[16];
	static int s_stage = -1;
	if (!s_read) {
		s_read = 1;
		char path[160];
		std::snprintf(path, sizeof(path), "%sdabr", sms_wiiu_app_root());
		if (FILE* f = std::fopen(path, "rb")) {
			int n = std::fscanf(f, "%31s %d %15s", s_tag, &s_stage, s_mode);
			if (n < 2)
				s_stage = -1;
			std::fclose(f);
		}
		port_log("[dabr] marker %s: tag=%s stage=%d mode=%s\n", path, s_stage >= 0 ? s_tag : "-", s_stage,
		         s_mode[0] ? s_mode : "-");
	}
	uint32_t a = (uint32_t)(uintptr_t)addr & ~7u;
	if (s_stage < 0 || stage != s_stage || std::strcmp(tag, s_tag) != 0) {
		port_log("[dabr] skip %s stage=%d addr=%08X\n", tag, stage, (unsigned)a);
		return;
	}
	BOOL ok = OSSetDABR(TRUE, a, FALSE, TRUE);
	s_watch_addr = a;
	port_log("[dabr] ARMED %s stage=%d addr=%08X ret=%d\n", tag, stage, (unsigned)a, (int)ok);
	if (std::strcmp(s_mode, "test") == 0) {
		/* Positive control: this store must raise a DSI naming port_dabr_watch. */
		port_log("[dabr] control store to %08X\n", (unsigned)a);
		volatile uint32_t* w = (volatile uint32_t*)(uintptr_t)a;
		*w = *w;
		port_log("[dabr] control store NOT caught\n");
	}
}

void sms_wiiu_runtime_init(void)
{
	if (s_initialized)
		return;

	uint32_t mem1_address = 0;
	uint32_t mem1_bytes = 0;
	/* Judge by the bound it reports, not the return code: Cemu 2.6 fills in
	 * MEM1 (0xF4000000, 32 MiB) but returns non-zero, and bailing out here
	 * left the game running with no arena. */
	OSGetMemBound(OS_MEM1, &mem1_address, &mem1_bytes);
	if (mem1_bytes <= kMem1Reserve) {
		WHBLogPrintf("SMS Wii U: OSGetMemBound(MEM1) failed");
		WHBProcStopRunning();
		return;
	}
	port_mem1_base = reinterpret_cast<uint8_t*>(
	    static_cast<uintptr_t>(mem1_address + kMem1Reserve));
	port_mem1_size = mem1_bytes - kMem1Reserve;

	uint32_t before = default_heap_free();
	void* arena = MEMAllocFromDefaultHeapEx(kGameArenaBytes, 0x40);
	if (!arena) {
		WHBLogPrintf("SMS Wii U: MEM2 game arena allocation failed (%u bytes)",
		             kGameArenaBytes);
		WHBProcStopRunning();
		return;
	}
	if (pointer_above_32(arena)) {
		port_ptr32_trap(arena);
		return;
	}
	sms_wiiu_mem2_base = reinterpret_cast<uint8_t*>(arena);
	sms_wiiu_mem2_size = kGameArenaBytes;
	/* Window at address 0: GX "physical" addresses are then the pointers
	 * themselves. J3DLoadArrayBasePtr and others write (u32)ptr straight into
	 * the FIFO, so an arena-relative window sent every J3D vertex array to the
	 * wrong place. The Wii U heap (0x1xxxxxxx) fits the decoder's 30-bit mask. */
	GXPC_SetMemoryWindow(nullptr, 0x40000000u);
	/* Cached display lists and decodes skip re-hashing pages that no cache
	 * flush has touched since; track the game arena for that. */
	GXPC_SetWriteStampWindow(arena);
	resolve_app_root();
	std::strncpy(s_files_root, kAppRootCandidates[kAppRootIndex],
	             sizeof(s_files_root));
	std::strncat(s_files_root, "files/", sizeof(s_files_root) -
	                                      std::strlen(s_files_root) - 1);
	port_disc_root = s_files_root;
	port_disc_explicit = 1;
	open_log();
	s_initialized = true;
	port_log("SMS Wii U: runtime start\n");
	port_log("SMS Wii U: MEM1 usable=%p size=%u (reserved=%u)\n",
	         port_mem1_base, port_mem1_size, kMem1Reserve);
	port_log("SMS Wii U: MEM2 game arena=%p size=%u default_free_before=%u "
	         "default_free_after=%u\n",
	         sms_wiiu_mem2_base, sms_wiiu_mem2_size, before,
	         default_heap_free());
	port_log("SMS Wii U: app root=%s data root=%s\n",
	         kAppRootCandidates[kAppRootIndex],
	         s_files_root);
}

void port_init(int, char**)
{
	if (s_initialized && s_heap_ready)
		return;
	if (const char* skip = std::getenv("SMS_WIIU_SKIP_INTRO"))
		port_skip_movies = *skip && std::strcmp(skip, "0") != 0;
	sms_wiiu_runtime_init();
	if (!s_initialized)
		return;
	port_os_init();
	port_os_threads_init();
	s_heap_ready = true;
	port_dvd_init();
	/* Sound: the DSP HLE (platform/audio) mixes on JAudio's audio thread and
	 * audio.cpp plays the DAC blocks through SDL2. SMS_NO_AUDIO=1 in the
	 * environment still selects the empty sound configuration. */
	port_noaudio_init();
	sms_wiiu_gfx_init();
	port_vi_init();
	port_pad_init();
	/* AX normally opens here on core 2 before SMS_main can race JAudio's DSP
	 * map creation with the pacer thread. axonc1, noaudio, and sdlaudio keep
	 * the preopen disabled; axcore2 remains accepted for compatibility. */
	if (!port_no_audio)
		sms_wiiu_sdl_audio_preopen(32000);
	port_log("SMS Wii U: target OS, threads, FST/DV, audio, VI, "
	         "and input initialized\n");
	port_stub_report();
}

void* port_low_alloc(unsigned long size)
{
	if (!size || size > sms_wiiu_mem2_size)
		return nullptr;
	void* result = MEMAllocFromDefaultHeapEx(static_cast<uint32_t>(size), 64);
	if (!result || pointer_above_32(result)) {
		port_ptr32_trap(result);
		return nullptr;
	}
	return result;
}

int64_t port_time_ticks(void)
{
	return static_cast<int64_t>(sms_wiiu_OSGetTime());
}

void* port_dvd_read_file(const char* path, size_t* size)
{
	return sms_wiiu_dvd_read_file(path, size);
}

void port_stub_hit(port_stub_rec* rec)
{
	if (!rec)
		return;
	if (rec->count++ == 0) {
		rec->next = s_stub_list;
		s_stub_list = rec;
		++s_stub_distinct;
		port_log("[stub] %s (first call; %u distinct stubs hit)\n", rec->name,
		         s_stub_distinct);
	}
}

void port_stub_report(void)
{
	port_log("[stub] summary: %u distinct SDK stubs were called\n",
	         s_stub_distinct);
}

void port_ptr32_trap(const void* ptr)
{
	port_log("[port] PTR32 address does not fit: %p\n", ptr);
	WHBProcStopRunning();
}

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

class TMarioGamePad;
namespace JDrama { struct TRect; }

void SaveStateMenuOpen() {}
void SaveStateMenuClose() {}
bool SaveStateMenuIsOpen() { return false; }
void SaveStateMenuMove(TMarioGamePad*) {}
void SaveStateMenuSetAlpha(unsigned char) {}
void SaveStateMenuDraw(const JDrama::TRect&) {}
void PauseTrackerReset() {}
void PauseTrackerMove(TMarioGamePad*) {}
void PauseTrackerDraw(const JDrama::TRect&, unsigned char) {}
