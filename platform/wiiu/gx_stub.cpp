#include "include/dolphin/gx/GXVert.h"
#include "../gx/include/sms_gx/gx_pc.h"
#include "runtime.h"

#include <dolphin/gx/GXCpu2Efb.h>
#include <dolphin/gx/GXFrameBuffer.h>
#include <dolphin/gx/GXManage.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/proc.h>
#include <proc_ui/procui.h>
#include <cstdlib>
#include <whb/gfx.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

uint32_t s_present_count;
float s_widescreen = 1.0f;
bool s_logged;
bool s_gfx_initialized;
bool s_frame_open;

extern "C" int sms_wiiu_gx2_renderer_init(void);
extern "C" void sms_wiiu_gx2_renderer_begin(void);
extern "C" void sms_wiiu_gx2_renderer_end(void);

void log_active_once()
{
	if (!s_logged) {
		s_logged = true;
		WHBLogPrintf("SMS Wii U: GX2 renderer active");
	}
}

} // namespace

extern "C" void sms_wiiu_gfx_init(void)
{
	if (s_gfx_initialized)
		return;
	s_gfx_initialized = WHBGfxInit() != FALSE;
	if (!s_gfx_initialized)
		WHBLogPrintf("SMS Wii U: WHBGfxInit failed");
	else if (!sms_wiiu_gx2_renderer_init())
		WHBLogPrintf("SMS Wii U: GX2 renderer setup failed");
	else {
		WHBGfxBeginRender();
		WHBGfxBeginRenderTV();
		WHBGfxClearColor(0.02f, 0.18f, 0.03f, 1.0f);
		sms_wiiu_gx2_renderer_begin();
		s_frame_open = true;
	}
}

/* Orderly exit on the main thread after ProcUI reported EXITING: WHBGfx owns
 * its foreground-release handling; release it, then end the process like a
 * return from main. */
extern "C" void sms_wiiu_exit(void)
{
	if (s_gfx_initialized) {
		if (s_frame_open) {
			sms_wiiu_gx2_renderer_end();
			s_frame_open = false;
		}
		s_gfx_initialized = false;
		WHBGfxShutdown();
	}
	WHBLogPrintf("SMS Wii U: exiting");
	WHBProcShutdown();
	WHBLogUdpDeinit();
	/* _Exit, not std::exit: the game's global destructors run while its other
	 * threads are still alive, and a relaunch froze right after "exiting". */
	std::_Exit(0);
}

extern "C" int sms_wiiu_gfx_is_initialized(void)
{
	return s_gfx_initialized ? 1 : 0;
}

extern "C" void sms_wiiu_gfx_frame(void)
{
	log_active_once();
	/* After RELEASE_FOREGROUND, WHBProcIsRunning still returns true while the app
	 * runs in the background; any GX2 call then freezes the console (DKR). */
	if (!s_gfx_initialized || !ProcUIInForeground())
		return;
	const bool loaded = std::strcmp(sms_wiiu_stage_path(), "unknown") != 0;
	const float r = loaded ? 0.02f : 0.0f;
	const float g = loaded ? 0.18f : 0.02f;
	const float b = loaded ? 0.03f : 0.16f;
	if (!s_frame_open) {
		WHBGfxBeginRender();
		WHBGfxBeginRenderTV();
		WHBGfxClearColor(0.02f, 0.18f, 0.03f, 1.0f);
		sms_wiiu_gx2_renderer_begin();
		s_frame_open = true;
	}
	sms_wiiu_gx2_renderer_end();
	WHBGfxFinishRenderTV();
	WHBGfxBeginRenderDRC();
	WHBGfxClearColor(r, g, b, 1.0f);
	WHBGfxFinishRenderDRC();
	WHBGfxFinishRender();
	WHBGfxBeginRender();
	WHBGfxBeginRenderTV();
	WHBGfxClearColor(0.02f, 0.18f, 0.03f, 1.0f);
	sms_wiiu_gx2_renderer_begin();
	s_frame_open = true;
}

extern "C" {

int GXPC_ParseArgs(int*, char**) { return 0; }
void GXPC_SetHeadless(int) {}
void GXPC_PrintDisplayInfo(void) {}
int GXPC_InitAuto(float scale) { log_active_once(); return sms_wiiu_gx2_renderer_init() ? 1 : 0; }
int GXPC_IsHeadless(void) { return 1; }
void GXPC_SetAutoPresent(int) {}
void GXPC_Present(const void*) { log_active_once(); ++s_present_count; sms_wiiu_frame_boundary(); }
void GXPC_SkipNextPresent(int) {}
double GXPC_LastPresentSeconds(void) { return 0.0; }
uint32_t GXPC_FrameCount(void) { return sms_wiiu_frame_count(); }
void GXPC_SetWindowIcon(const uint8_t*, int, int) {}
int GXPC_MouseCaptured(void) { return 0; }
int GXPC_GameInputBlocked(void) { return 0; }
void GXPC_SetWidescreen(float width) { s_widescreen = width; }
void GXPC_SetStretch2D(int) {}
void GXPC_SetWide2D(int) {}
void GXPC_SetHud(int) {}
void GXPC_HudPaneBegin(float, float) {}
void GXPC_HudPaneEnd(void) {}
float GXPC_GetWidescreen(void) { return s_widescreen; }
void sms_gx_set_event_callback(void (*)(const union SDL_Event*)) {}
void sms_gx_pump_events(void) {}
int sms_gx_rumble(unsigned short) { return 0; }
int GXPC_Init(GXPCGetProcFn, float) { log_active_once(); return 1; }
void GXPC_Shutdown(void) {}
void GXPC_PrefetchResource(const void*, uint32_t, const char*) {}
void GXPC_PreloadTextures(void) {}
void GXPC_PrepareStage(int) {}

int GXPC_PresentXFB(const void*, int, int)
{
	log_active_once();
	++s_present_count;
	sms_wiiu_frame_boundary();
	return 1;
}

void GXPC_EndPresent(void) {}
void GXPC_ReadEFB(uint8_t*, int* w, int* h)
{
	if (w) *w = 0;
	if (h) *h = 0;
}
int GXPC_ReadXFB(const void*, uint8_t*, int* w, int* h)
{
	if (w) *w = 0;
	if (h) *h = 0;
	return 0;
}
void GXPC_GetStats(GXPCStats* out) { if (out) std::memset(out, 0, sizeof(*out)); }
void GXPC_GetLastFrameStats(GXPCStats* out) { if (out) std::memset(out, 0, sizeof(*out)); }
double GXPC_GxSeconds(void) { return 0.0; }
void GXPC_GetTimes(GXPCTimes* out) { if (out) std::memset(out, 0, sizeof(*out)); }
void GXPC_SetDetailedTimers(int) {}
void GXPC_SetIdleClock(double (*)(void)) {}
void GXPC_DrawOverlay(const uint8_t*, int, int, int, int, int, int, int) {}
void GXPC_OverlayToggle(void) {}
void GXPC_OverlayDraw(int, int) {}
void GXPC_OverlayFrame(void) {}
int GXPC_OverlayVisible(void) { return 0; }
void GXPC_CycleSpeed(void) {}
int GXPC_GetSpeed(void) { return 1; }

/* Ancillary weak interfaces used only when optional host menu sources slip
 * into a source list.  Stage 1 has no screenshots or prompt device. */
__attribute__((weak)) int GXPC_InputStyle(void) { return 0; }
__attribute__((weak)) uint8_t* GXPC_SaveStateThumbnail(size_t* size)
{
	if (size) *size = 0;
	return nullptr;
}
__attribute__((weak)) void GXPC_SetSaveStateApi(const void*) {}

} // extern "C"
