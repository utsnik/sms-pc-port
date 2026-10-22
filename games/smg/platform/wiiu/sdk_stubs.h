#pragma once

/*
 * Stage 1 has no Wii data or game input service. These are the residual
 * link-only symbols allowed to fail safely until their later owner is
 * implemented. Keep this list finite and reviewable; never replace it with
 * a wildcard or a weak-definition blanket.
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
  X(SCGetAspectRatio) \
  X(SCGetProgressiveMode) \
  X(SCGetLanguage) \
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
  X(smg_gc_AIInit) \
  X(smg_gc_AICheckInit) \
  X(smg_gc_AIReset) \
  X(smg_gc_AIRegisterDMACallback) \
  X(smg_gc_AIInitDMA) \
  X(smg_gc_AIGetDMAEnableFlag) \
  X(smg_gc_AIStartDMA) \
  X(smg_gc_AIStopDMA) \
  X(smg_gc_AIGetDMABytesLeft) \
  X(smg_gc_DSPInit) \
  X(smg_gc_DSPCheckInit) \
  X(smg_gc_DSPReset) \
  X(smg_gc_DSPHalt) \
  X(smg_gc_DSPUnhalt) \
  X(smg_gc_DSPAddTask) \
  X(__DSP_boot_task) \
  X(__cxa_pure_virtual) \
  X(__syscall_tls_create) \
  X(__syscall_tls_delete) \
  X(__syscall_tls_get) \
  X(__syscall_tls_set)

#ifdef __cplusplus
extern "C" {
#endif

void smg_stage1_stub_log_once(const char* symbol);

#ifdef __cplusplus
}
#endif
