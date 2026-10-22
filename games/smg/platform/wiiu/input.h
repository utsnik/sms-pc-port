#pragma once

/* Galaxy owns this input boundary.  The shared Sunshine PAD provider is a
 * GameCube PADStatus adapter and cannot represent a Wii pointer. */
#include <revolution/kpad.h>
#include <revolution/wpad.h>
#include <vpad/input.h>

#ifndef KPAD_ERROR_OK
#define KPAD_ERROR_OK 0
#define KPAD_ERROR_NO_SAMPLES (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* wut declares this in padscore/kpad.h, while the game uses the Revolution
 * KPADStatus layout above.  The ABI is the same four-argument C call. */
uint32_t KPADReadEx(s32 chan, KPADStatus* status, uint32_t count, s32* error);

#ifdef __cplusplus
}
#endif
