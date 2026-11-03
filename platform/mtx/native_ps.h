#ifndef SMS_PLATFORM_MTX_NATIVE_PS_H
#define SMS_PLATFORM_MTX_NATIVE_PS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The SD-controlled Espresso paired-single fast path.  The native path is
 * deliberately opt-in; the C implementation remains the default. */
void sms_wiiu_nativeps_init(int nativeps, int nativepscheck);

#ifdef __cplusplus
}
#endif

#endif
