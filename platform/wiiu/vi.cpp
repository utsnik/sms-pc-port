#include "runtime.h"
extern "C" uint32_t sms_wiiu_watch_word(void);
extern "C" uint32_t sms_port_heap_fallbacks(void);
extern "C" uint32_t sms_port_heap_fallback_bytes(void);

#include "port_os.h"
#include "port_framerate.h"

#include <dolphin/vi.h>
#include <whb/proc.h>
#include <gx2/event.h>
#include <whb/log.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" volatile int sms_wiiu_menu_exit_pending;

extern "C" {
void sms_wiiu_gfx_frame(void);
int sms_wiiu_gfx_is_initialized(void);
void sms_wiiu_exit(void);
void sms_wiiu_dump_threads(void);
void sms_wiiu_profiler_tick(void);
void sms_wiiu_profiler_stop(void);
void sms_wiiu_dvd_pump(void);
void sms_wiiu_crash_return_to_menu(void);
extern volatile int sms_wiiu_crash_pending;
/* wut symbols bound by name: the decomp's OSThread types clash with coreinit's. */
void* sms_wiiu_native_current_thread(void) __asm__("OSGetCurrentThread");
void sms_wiiu_native_sleep_ticks(int64_t ticks) __asm__("OSSleepTicks");
}

namespace {

u32 s_retrace_count;
u32 s_frame_count;
u32 s_last_heartbeat;
bool s_flush_pumped;
bool s_initialized;
void* s_next_framebuffer;
void* s_current_framebuffer;
bool s_black = true;
VIRetraceCallback s_pre_callback;
VIRetraceCallback s_post_callback;
char s_stage_path[512] = "unknown";
bool s_soak_reported;
bool s_exit_logged;
u32 s_offthread_logged;
/* GX2 and ProcUI belong to the thread that ran port_vi_init (the main thread on
 * core 1); a frame boundary reached from any other thread must not touch them. */
void* s_owner_thread;
/* One 60 Hz frame of the 62.15625 MHz timebase. */
constexpr int64_t kFrameTicks = 62156250 / 60;

} // namespace

extern "C" {

void* sms_wiiu_vi_owner_thread(void) { return s_owner_thread; }

void port_vi_init(void)
{
	if (s_initialized)
		return;
	s_initialized = true;
	s_retrace_count = 0;
	s_frame_count = 0;
	s_last_heartbeat = 0;
	s_owner_thread = sms_wiiu_native_current_thread();
	port_log("[vi] Wii U retrace boundary ready; WHBGfx=%s\n",
	         sms_wiiu_gfx_is_initialized() ? "ready" : "missing");
}

void sms_wiiu_frame_boundary(void)
{
	if (!s_initialized)
		return;
	if (sms_wiiu_native_current_thread() != s_owner_thread) {
		if (s_offthread_logged++ < 4)
			port_log("[vi] frame boundary from a non-main thread; sleeping one frame\n");
		sms_wiiu_native_sleep_ticks(kFrameTicks);
		return;
	}
	/* After EXITING, WHBProcIsRunning must never be called again (it blocks
	 * forever) and GX2 must not be touched: shut down and leave. */
	if (s_exit_logged)
		return;
	/* A game thread crashed (exception.cpp parked it): leave for the Menu
	 * from here, the only thread allowed to pump ProcUI. */
	if (sms_wiiu_crash_pending) {
		s_exit_logged = true;
		sms_wiiu_profiler_stop();
		sms_wiiu_crash_return_to_menu();
		return;
	}
	/* A Menu exit froze the console once (2026-10-19 21:49) during the
	 * foreground release inside the pump. Drain the GPU first and log each
	 * step: a hang inside GX2DrawDone means a draw had already hung the GPU. */
	if (sms_wiiu_menu_exit_pending == 1) {
		sms_wiiu_menu_exit_pending = 2;
		WHBLogPrintf("SMS Wii U: exit drain: submitted=%llu retired=%llu",
		             (unsigned long long)GX2GetLastSubmittedTimeStamp(),
		             (unsigned long long)GX2GetRetiredTimeStamp());
		GX2DrawDone();
		WHBLogPrintf("SMS Wii U: exit drain: done retired=%llu; pumping ProcUI",
		             (unsigned long long)GX2GetRetiredTimeStamp());
	}
	/* WHBProcIsRunning owns the sole ProcUI pump after SMS_main starts. */
	if (!WHBProcIsRunning()) {
		s_exit_logged = true;
		sms_wiiu_profiler_stop();
		port_log("SMS Wii U: ProcUI exit requested; shutting down\n");
		sms_wiiu_exit();
		return;
	}
	sms_wiiu_profiler_tick();
	port_irq_check();
	/* Deliver queued DVD async-read callbacks (GameCube ran them from the DVD
	 * interrupt); nothing else calls the pump. */
	sms_wiiu_dvd_pump();
	sms_wiiu_input_poll();
	if (s_pre_callback)
		s_pre_callback(s_retrace_count + 1);
	sms_wiiu_gfx_frame();
	++s_retrace_count;
	++s_frame_count;
	s_current_framebuffer = s_next_framebuffer;
	if (s_post_callback)
		s_post_callback(s_retrace_count);

	std::strncpy(s_stage_path, port_dvd_stage_path(), sizeof(s_stage_path) - 1);
	s_stage_path[sizeof(s_stage_path) - 1] = '\0';
	/* During boot, show where every game thread is waiting. */
	if (s_frame_count % 600 == 0 && s_frame_count <= 3600)
		sms_wiiu_dump_threads();
	if (s_frame_count - s_last_heartbeat >= 60) {
		s_last_heartbeat = s_frame_count;
		port_log("SMS Wii U: frame=%u stage=%s heap_free=%u port_fallbacks=%u/%u dvd=%s\n",
		         s_frame_count, s_stage_path, sms_wiiu_heap_free(),
		         sms_port_heap_fallbacks(), sms_port_heap_fallback_bytes(),
		         port_dvd_busy() ? "busy" : "idle");
	}
	/* Crash hunt: report the watched word turning into a non-pointer. */
	{
		static uint32_t s_last, s_reports;
		const uint32_t w = sms_wiiu_watch_word();
		const bool ok = w == 0 || (w >= 0x10000000u && w < 0x20000000u && !(w & 3u));
		if (w != s_last && !ok && s_reports < 8) {
			++s_reports;
			port_log("[watch] frame %u: word %08x -> %08x\n", s_frame_count, s_last, w);
		}
		s_last = w;
	}
	/* SMS_CRASHTEST=<frame> (autoplay file): a deliberate DSI on the main
	 * thread, to prove the crash path returns to the Menu unattended. */
	static int s_crash_test = -1;
	if (s_crash_test < 0) {
		const char* v = std::getenv("SMS_CRASHTEST");
		s_crash_test = v ? std::atoi(v) : 0;
	}
	if (s_crash_test > 0 && s_frame_count >= u32(s_crash_test)) {
		port_log("SMS Wii U: crash test at frame %u\n", s_frame_count);
		s_crash_test = 0;
		*reinterpret_cast<volatile u32*>(uintptr_t(0x8)) = 0xDEADu;
	}
	if (!s_soak_reported && s_frame_count >= 18000) {
		s_soak_reported = true;
		port_log("SMS Wii U: Stage 2 SOAK COMPLETE; GX link stub active\n");
	}
}

u32 sms_wiiu_frame_count(void) { return s_frame_count; }
const char* sms_wiiu_stage_path(void) { return s_stage_path; }

void VIInit(void) { port_vi_init(); }
void VIConfigure(GXRenderModeObj*) {}
void VIConfigurePan(u16, u16, u16, u16) {}
void VIFlush(void)
{
	if (!s_flush_pumped) {
		sms_wiiu_frame_boundary();
		s_flush_pumped = true;
	}
}
void VISetNextFrameBuffer(void* buffer) { s_next_framebuffer = buffer; }
void VISetNextRightFrameBuffer(void*) {}
void VISetBlack(BOOL black) { s_black = black != 0; }
void VISet3D(BOOL) {}
u32 VIGetRetraceCount(void) { return s_retrace_count; }
u32 VIGetNextField(void) { return (s_retrace_count & 1) + 1; }
u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetDTVStatus(void) { return 0; }
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback old = s_pre_callback;
	s_pre_callback = callback;
	return old;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback old = s_post_callback;
	s_post_callback = callback;
	return old;
}
void VIWaitForRetrace(void)
{
	if (!s_flush_pumped)
		sms_wiiu_frame_boundary();
	s_flush_pumped = false;
}

s32 port_vi_frame_wait(s32 due, u16 frames)
{
	while (static_cast<s32>(VIGetRetraceCount()) < due)
		VIWaitForRetrace();
	return static_cast<s32>(VIGetRetraceCount()) + frames;
}
void* port_vi_current_fb(void) { return s_current_framebuffer; }

} // extern "C"
