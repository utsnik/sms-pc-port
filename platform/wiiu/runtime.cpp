#include "runtime.h"

#include "port_stub.h"
#include "port_os.h"

#include <coreinit/memory.h>
#include <coreinit/memblockheap.h>
#include <coreinit/memdefaultheap.h>
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
const char* const kAppRootCandidates[] = {
	"fs:/vol/external01/wiiu/apps/sms/",
	"wiiu/apps/sms/",
	"/vol/external01/wiiu/apps/sms/",
};
const char* kAppRoot = kAppRootCandidates[0];
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
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
	return heap ? MEMGetAllocatableSizeForBlockHeapEx(heap, 0x40) : 0;
}

void resolve_app_root()
{
	for (const char* root : kAppRootCandidates) {
		char probe[160];
		std::snprintf(probe, sizeof(probe), "%ssys/fst.bin", root);
		FILE* file = std::fopen(probe, "rb");
		WHBLogPrintf("SMS Wii U: app root probe %s -> %s", root, file ? "ok" : "missing");
		if (file) {
			std::fclose(file);
			kAppRoot = root;
			return;
		}
	}
	WHBLogPrintf("SMS Wii U: no app root holds sys/fst.bin; keeping %s", kAppRoot);
}

void open_log()
{
	/* No SD log: a console wedge needs a forced power-off, and a file left open
	 * for writing then can damage the card's filesystem (2026-10-17 lost an
	 * Aroma file that way). Logs go over UDP only. */
	return;
	char path[160];
	std::snprintf(path, sizeof(path), "%sstage2.log", kAppRoot);
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
int port_skip_movies = 1;
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

const char* sms_wiiu_app_root(void) { return kAppRoot; }

uint32_t sms_wiiu_heap_free(void) { return default_heap_free(); }

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

void sms_wiiu_runtime_init(void)
{
	if (s_initialized)
		return;

	uint32_t mem1_address = 0;
	uint32_t mem1_bytes = 0;
	if (OSGetMemBound(OS_MEM1, &mem1_address, &mem1_bytes) != 0 ||
	    mem1_bytes <= kMem1Reserve) {
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
	resolve_app_root();
	std::strncpy(s_files_root, kAppRoot, sizeof(s_files_root));
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
	port_log("SMS Wii U: app root=%s data root=%s\n", kAppRoot,
	         s_files_root);
}

void port_init(int, char**)
{
	if (s_initialized && s_heap_ready)
		return;
	sms_wiiu_runtime_init();
	if (!s_initialized)
		return;
	port_os_init();
	port_os_threads_init();
	s_heap_ready = true;
	port_dvd_init();
	/* Stage 2 is silent: the AI DMA engine is not emulated yet, and JAudio's
	 * real path waits on its interrupt forever. SMS_NO_AUDIO gives JAudio the
	 * PC port's empty sound configuration and leaves DMA idle. */
	setenv("SMS_NO_AUDIO", "1", 1);
	port_noaudio_init();
	sms_wiiu_gfx_init();
	port_vi_init();
	port_pad_init();
	port_log("SMS Wii U: target OS, threads, FST/DV, silent audio, VI, "
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
