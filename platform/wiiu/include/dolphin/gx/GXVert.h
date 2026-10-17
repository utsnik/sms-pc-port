#ifndef _DOLPHIN_GX_GXVERT_H_
#define _DOLPHIN_GX_GXVERT_H_

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 1 has no GX2 backend.  Direct FIFO stores and all convenience
 * writers land in this ordinary object and therefore have no hardware side
 * effects.  The interface remains source-compatible with Dolphin GXVert.h. */
typedef union PPCWGPipe {
	u8 u8;
	u16 u16;
	u32 u32;
	u64 u64;
	s8 s8;
	s16 s16;
	s32 s32;
	s64 s64;
	f32 f32;
	f64 f64;
} PPCWGPipe;

extern volatile PPCWGPipe GXWGFifo;

#define SMS_WIIU_FIFO_1(name, type, member) \
	static inline void name##1##type(type x) { GXWGFifo.member = x; }
#define SMS_WIIU_FIFO_2(name, type, member) \
	static inline void name##2##type(type x, type y) { GXWGFifo.member = x; GXWGFifo.member = y; }
#define SMS_WIIU_FIFO_3(name, type, member) \
	static inline void name##3##type(type x, type y, type z) \
	{ GXWGFifo.member = x; GXWGFifo.member = y; GXWGFifo.member = z; }
#define SMS_WIIU_FIFO_4(name, type, member) \
	static inline void name##4##type(type x, type y, type z, type w) \
	{ GXWGFifo.member = x; GXWGFifo.member = y; GXWGFifo.member = z; GXWGFifo.member = w; }
#define SMS_WIIU_FIFO_INDEX8(name) \
	static inline void name##1x8(u8 x) { GXWGFifo.u8 = x; }
#define SMS_WIIU_FIFO_INDEX16(name) \
	static inline void name##1x16(u16 x) { GXWGFifo.u16 = x; }

SMS_WIIU_FIFO_1(GXCmd, u8, u8)
SMS_WIIU_FIFO_1(GXCmd, u16, u16)
SMS_WIIU_FIFO_1(GXCmd, u32, u32)
SMS_WIIU_FIFO_1(GXParam, u8, u8)
SMS_WIIU_FIFO_1(GXParam, u16, u16)
SMS_WIIU_FIFO_1(GXParam, u32, u32)
SMS_WIIU_FIFO_1(GXParam, s8, s8)
SMS_WIIU_FIFO_1(GXParam, s16, s16)
SMS_WIIU_FIFO_1(GXParam, s32, s32)
SMS_WIIU_FIFO_1(GXParam, f32, f32)
SMS_WIIU_FIFO_3(GXParam, f32, f32)
SMS_WIIU_FIFO_4(GXParam, f32, f32)
SMS_WIIU_FIFO_3(GXPosition, f32, f32)
SMS_WIIU_FIFO_3(GXPosition, u8, u8)
SMS_WIIU_FIFO_3(GXPosition, s8, s8)
SMS_WIIU_FIFO_3(GXPosition, u16, u16)
SMS_WIIU_FIFO_3(GXPosition, s16, s16)
SMS_WIIU_FIFO_2(GXPosition, f32, f32)
SMS_WIIU_FIFO_2(GXPosition, u8, u8)
SMS_WIIU_FIFO_2(GXPosition, s8, s8)
SMS_WIIU_FIFO_2(GXPosition, u16, u16)
SMS_WIIU_FIFO_2(GXPosition, s16, s16)
SMS_WIIU_FIFO_INDEX16(GXPosition)
SMS_WIIU_FIFO_INDEX8(GXPosition)
SMS_WIIU_FIFO_3(GXNormal, f32, f32)
SMS_WIIU_FIFO_3(GXNormal, s16, s16)
SMS_WIIU_FIFO_3(GXNormal, s8, s8)
SMS_WIIU_FIFO_INDEX16(GXNormal)
SMS_WIIU_FIFO_INDEX8(GXNormal)
SMS_WIIU_FIFO_4(GXColor, u8, u8)
SMS_WIIU_FIFO_1(GXColor, u32, u32)
SMS_WIIU_FIFO_3(GXColor, u8, u8)
SMS_WIIU_FIFO_1(GXColor, u16, u16)
SMS_WIIU_FIFO_INDEX16(GXColor)
SMS_WIIU_FIFO_INDEX8(GXColor)
SMS_WIIU_FIFO_2(GXTexCoord, f32, f32)
SMS_WIIU_FIFO_2(GXTexCoord, s16, s16)
SMS_WIIU_FIFO_2(GXTexCoord, u16, u16)
SMS_WIIU_FIFO_2(GXTexCoord, s8, s8)
SMS_WIIU_FIFO_2(GXTexCoord, u8, u8)
SMS_WIIU_FIFO_1(GXTexCoord, f32, f32)
SMS_WIIU_FIFO_1(GXTexCoord, s16, s16)
SMS_WIIU_FIFO_1(GXTexCoord, u16, u16)
SMS_WIIU_FIFO_1(GXTexCoord, s8, s8)
SMS_WIIU_FIFO_1(GXTexCoord, u8, u8)
SMS_WIIU_FIFO_INDEX16(GXTexCoord)
SMS_WIIU_FIFO_INDEX8(GXTexCoord)
SMS_WIIU_FIFO_1(GXMatrixIndex, u8, u8)

#undef SMS_WIIU_FIFO_1
#undef SMS_WIIU_FIFO_2
#undef SMS_WIIU_FIFO_3
#undef SMS_WIIU_FIFO_4
#undef SMS_WIIU_FIFO_INDEX8
#undef SMS_WIIU_FIFO_INDEX16

#ifdef __cplusplus
}
#endif

#endif
