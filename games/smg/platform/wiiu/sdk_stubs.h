#pragma once

/*
 * Stage 1 has no Wii data service. KPAD/WPAD are intentionally absent here:
 * input.cpp is their Galaxy owner. These are the remaining residual link-only
 * symbols allowed to fail safely until a later owner is implemented. Keep
 * this list finite and reviewable; never replace it with a wildcard or a
 * weak-definition blanket.
 */
#define SMG_STAGE1_STUB_SYMBOLS(X) \
  X(OSSetErrorHandler) \
  X(OSProtectRange) \
  X(OSFillFPUContext) \
  X(OSRegisterVersion) \
  X(OSSetPowerCallback) \
  X(OSShutdownSystem) \
  X(OSReturnToMenu) \
  X(OSRestart) \
  X(OSRebootSystem) \
  X(VISetTrapFilter) \
  X(VIGetCurrentFrameBuffer) \
  X(VIGetNextFrameBuffer) \
  X(VIEnableDimming) \
  X(VIGetDimmingCount) \
  X(SCGetSoundMode) \
  X(SCGetProgressiveMode) \
  X(SCGetEuRgb60Mode) \
  X(RFLSearchOfficialData) \
  X(RFLGetAdditionalInfo) \
  X(RFLSetExpression) \
  X(RFLMakeIcon) \
  X(RFLIsAvailableOfficialData) \
  X(RFLSetMtx) \
  X(RFLLoadVertexSetting) \
  X(RFLLoadMaterialSetting) \
  X(RFLInitResAsync) \
  X(RFLInitCharModel) \
  X(RFLGetWorkSize) \
  X(RFLGetModelBufferSize) \
  X(RFLGetLastReason) \
  X(RFLGetAsyncStatus) \
  X(RFLExit) \
  X(RFLDrawXluCore) \
  X(RFLDrawXlu) \
  X(RFLDrawOpaCore) \
  X(RFLDrawOpa) \
  X(NWC24SetMsgAttached) \
  X(NWC24SetMsgToId) \
  X(NWC24SetMsgText) \
  X(NWC24SetMsgTag) \
  X(NWC24SetMsgMBNoReply) \
  X(NWC24SetMsgMBDelay) \
  X(NWC24SetMsgLedPattern) \
  X(NWC24SetMsgAltName) \
  X(THPInit) \
  X(THPVideoDecode) \
  X(THPAudioDecode) \
  X(VFInitEx) \
  X(NWC24InitMsgObj) \
  X(NWC24GetMyUserId) \
  X(NWC24CommitMsg) \
  X(NWC24GetMsgSize) \
  X(NWC24OpenLib) \
  X(NWC24GetErrorCode) \
  X(NWC24CloseLib) \
  X(LCEnable) \
  X(LCDisable) \
  X(DVDCheckDiskAsync) \
  X(smg_gc_DSPCheckInit) \
  X(smg_gc_DSPReset) \
  X(smg_gc_DSPHalt) \
  X(smg_gc_DSPUnhalt) \
  X(smg_gc_DSPAddTask) \
  X(__cxa_pure_virtual) \
  X(__syscall_tls_create) \
  X(__syscall_tls_delete) \
  X(__syscall_tls_get) \
  X(__syscall_tls_set)

#define SMG_STAGE1_VALUE_STUB_SYMBOLS(X) \
  X(SCGetAspectRatio, SC_ASPECT_RATIO_16x9)

#ifdef __cplusplus
extern "C" {
#endif

void smg_stage1_stub_log_once(const char* symbol);

#ifdef __cplusplus
}
#endif
