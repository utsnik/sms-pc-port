#include "include/dolphin/gx/GXVert.h"
#include "../gx/include/sms_gx/gx_pc.h"

#include <whb/log.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

uint8_t s_pipe_sink[64];
uint8_t* s_memory_base;
uint32_t s_memory_size;
uint32_t s_frame_count;
uint32_t s_present_count;
float s_widescreen = 1.0f;
bool s_logged;

void log_active_once()
{
	if (!s_logged) {
		s_logged = true;
		WHBLogPrintf("SMS Wii U: GX link stub active");
	}
}

} // namespace

extern "C" {

volatile PPCWGPipe GXWGFifo = {};
uint8_t* GXPC_PipeBuf = s_pipe_sink;
uint32_t GXPC_PipeLen = 0;
uint32_t GXPC_PipeFast = 0;

int GXPC_ParseArgs(int*, char**) { return 0; }
void GXPC_SetHeadless(int) {}
void GXPC_PrintDisplayInfo(void) {}
int GXPC_InitAuto(float) { log_active_once(); return 1; }
int GXPC_IsHeadless(void) { return 1; }
void GXPC_SetAutoPresent(int) {}
void GXPC_Present(const void*) { log_active_once(); ++s_present_count; ++s_frame_count; }
void GXPC_SkipNextPresent(int) {}
double GXPC_LastPresentSeconds(void) { return 0.0; }
uint32_t GXPC_FrameCount(void) { return s_frame_count; }
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

void GXPC_SetMemoryWindow(void* base, uint32_t size)
{
	s_memory_base = static_cast<uint8_t*>(base);
	s_memory_size = size;
}

uint32_t GXPC_PtrToPhys(const void* ptr)
{
	if (!s_memory_base || !ptr)
		return 0;
	const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
	const uintptr_t base = reinterpret_cast<uintptr_t>(s_memory_base);
	if (address < base)
		return 0;
	const uintptr_t offset = address - base;
	return offset < s_memory_size ? static_cast<uint32_t>(offset) : 0;
}

void* GXPC_PhysToPtr(uint32_t phys)
{
	return s_memory_base && phys < s_memory_size ? s_memory_base + phys : nullptr;
}

void GXPC_InvalidateRange(const void*, uint32_t) {}
void GXPC_MemoryWritten(const void*, uint32_t) {}
void GXPC_SetArrayBigEndian(int, int) {}
void GXPC_SetDefaultArrayBigEndian(int) {}
void GXPC_AddBigEndianRange(const void*, uint32_t) {}
void GXPC_RemoveBigEndianRange(const void*) {}
void GXPC_Write8(uint8_t) {}
void GXPC_Write16(uint16_t) {}
void GXPC_Write32(uint32_t) {}
void GXPC_WriteF32(float) {}

int GXPC_PresentXFB(const void*, int, int)
{
	log_active_once();
	++s_present_count;
	++s_frame_count;
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
