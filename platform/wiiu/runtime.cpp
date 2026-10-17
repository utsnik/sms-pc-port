#include "runtime.h"

#include "port_stub.h"

#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/proc.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kArenaBytes = 20u * 1024u * 1024u;
constexpr uint32_t kLowPoolBytes = 2u * 1024u * 1024u;
alignas(64) uint8_t s_arena[kArenaBytes];
uint32_t s_low_used;
bool s_initialized;
port_stub_rec* s_stub_list;
unsigned s_stub_distinct;

} // namespace

extern "C" {

uint8_t* port_mem1_base = s_arena;
uint32_t port_mem1_size = kArenaBytes - kLowPoolBytes;

const char* port_disc_root = nullptr;
int port_disc_explicit = 0;
int port_no_audio = 1;
int port_skip_movies = 1;
int port_heat_haze = 0;
int port_pause_tracker = 0;
float port_widescreen = 1.0f;
int port_frame_rate = 30;
int port_active_frame_rate = 30;
int port_high_fps_active = 0;

void port_log(const char* fmt, ...)
{
	char line[768];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	line[sizeof(line) - 1] = '\0';
	WHBLogPrintf("%s", line);
}

void sms_wiiu_runtime_init(void)
{
	if (s_initialized)
		return;
	s_initialized = true;
	std::memset(s_arena, 0, sizeof(s_arena));
	port_log("SMS Wii U: runtime stub arena=%u bytes\n", port_mem1_size);
}

void port_init(int, char**)
{
	sms_wiiu_runtime_init();
	port_log("SMS Wii U: Stage 1 runtime initialized\n");
}

void* port_low_alloc(unsigned long size)
{
	const uint32_t aligned = (static_cast<uint32_t>(size) + 63u) & ~63u;
	if (size > kLowPoolBytes || aligned > kLowPoolBytes - s_low_used)
		return nullptr;
	uint8_t* result = s_arena + port_mem1_size + s_low_used;
	s_low_used += aligned;
	return result;
}

int64_t port_time_ticks(void)
{
	return static_cast<int64_t>(OSGetTime());
}

void* port_dvd_read_file(const char*, size_t* size)
{
	if (size)
		*size = 0;
	return nullptr;
}

void port_stub_hit(port_stub_rec* rec)
{
	if (!rec)
		return;
	if (rec->count++ == 0) {
		rec->next = s_stub_list;
		s_stub_list = rec;
		++s_stub_distinct;
		port_log("[stub] %s (first call; %u distinct stubs hit)\n", rec->name, s_stub_distinct);
	}
}

void port_stub_report(void)
{
	port_log("[stub] summary: %u distinct SDK stubs were called\n", s_stub_distinct);
}

void port_ptr32_trap(const void* ptr)
{
	port_log("[port] PTR32 address does not fit: %p\n", ptr);
	WHBProcStopRunning();
}

/* These weak Stage 1 hooks cover common-source references without competing
 * with a later native implementation selected by the target source list. */
__attribute__((weak)) void port_os_init(void) {}
__attribute__((weak)) void port_os_threads_init(void) {}
__attribute__((weak)) void port_dvd_init(void) {}
__attribute__((weak)) void port_vi_init(void) {}
__attribute__((weak)) void port_noaudio_init(void) {}
__attribute__((weak)) uint8_t* port_noaudio_init_data(void) { return nullptr; }
__attribute__((weak)) void port_window_icon_init(void) {}
__attribute__((weak)) void port_irq_check(void) {}
__attribute__((weak)) void port_irq_kick(void) {}
__attribute__((weak)) void port_irq_add_source(void (*)(void)) {}
__attribute__((weak)) void port_os_run_lower(double) {}
__attribute__((weak)) double port_idle_seconds(void) { return 0.0; }
__attribute__((weak)) int port_vi_idle_advance(void) { return 0; }
__attribute__((weak)) int port_vi_deterministic(void) { return 0; }
__attribute__((weak)) int64_t port_vi_virtual_ticks(void) { return port_time_ticks(); }
__attribute__((weak)) void port_audio_on_retrace(void) {}
__attribute__((weak)) void port_pad_autopress_field(uint32_t) {}
__attribute__((weak)) int port_camera_take_mouse(float* dx, float* dy)
{
	if (dx)
		*dx = 0.0f;
	if (dy)
		*dy = 0.0f;
	return 0;
}
__attribute__((weak)) int port_dvd_busy(void) { return 0; }
__attribute__((weak)) int port_dvd_override(const char*, const void*, uint32_t) { return 0; }
__attribute__((weak)) const char* port_card_dir(void) { return nullptr; }
__attribute__((weak)) int port_setenv(const char*, const char*, int) { return -1; }
__attribute__((weak)) int port_mkdir(const char*, int) { return -1; }

} // extern "C"
