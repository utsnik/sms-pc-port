#pragma once

/*
 * Stage 1 has no Wii data, NAND, or game input service.  These are the
 * residual link-only symbols allowed to fail safely until their later owner
 * is implemented.  Keep this list finite and reviewable; never replace it
 * with a wildcard or a weak-definition blanket.
 */
#define SMG_STAGE1_STUB_SYMBOLS(X) \
  X(KPADInit) \
  X(KPADRead) \
  X(KPADReset) \
  X(KPADSetAccParam) \
  X(KPADSetBtnRepeat) \
  X(KPADSetDistParam) \
  X(KPADSetHoriParam) \
  X(KPADSetPosParam) \
  X(KPADSetSensorHeight) \
  X(WPADInit) \
  X(WPADShutdown) \
  X(WPADRead) \
  X(WPADProbe) \
  X(WPADSetDataFormat) \
  X(WPADSetVRes) \
  X(WPADDisconnect) \
  X(NANDInit) \
  X(NANDCheck) \
  X(NANDOpen) \
  X(NANDClose) \
  X(NANDRead) \
  X(NANDWrite) \
  X(NANDCreate) \
  X(NANDDelete) \
  X(NANDMove) \
  X(NANDGetStatus) \
  X(NANDGetHomeDir) \
  X(NANDGetLength) \
  X(NANDInitBanner) \
  X(GXPeekARGB) \
  X(GXPeekZ) \
  X(GXPerf0) \
  X(GXPerf1)

#ifdef __cplusplus
extern "C" {
#endif

void smg_stage1_stub_log_once(const char* symbol);

#ifdef __cplusplus
}
#endif
