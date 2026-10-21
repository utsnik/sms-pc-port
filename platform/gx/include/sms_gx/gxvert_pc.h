/* PC replacement for the body of dolphin/gx/GXVert.h. */
#ifndef SMS_GX_GXVERT_PC_H
#define SMS_GX_GXVERT_PC_H

#include <dolphin/types.h>
#include <sms_gx/gx_pc.h>

#ifdef __cplusplus
#define GXWGFifo GXPC_WGPipe
extern "C" {
#endif

static inline void GXCmd1u8(u8 x) { GXPC_Put8(x); }
static inline void GXCmd1u16(u16 x) { GXPC_Put16(x); }
static inline void GXCmd1u32(u32 x) { GXPC_Put32((uint32_t)x); }
static inline void GXParam1u8(u8 x) { GXPC_Put8(x); }
static inline void GXParam1u16(u16 x) { GXPC_Put16(x); }
static inline void GXParam1u32(u32 x) { GXPC_Put32((uint32_t)x); }
static inline void GXParam1s8(s8 x) { GXPC_Put8((uint8_t)x); }
static inline void GXParam1s16(s16 x) { GXPC_Put16((uint16_t)x); }
static inline void GXParam1s32(s32 x) { GXPC_Put32((uint32_t)x); }
static inline void GXParam1f32(f32 x) { GXPC_PutF32(x); }
static inline void GXParam3f32(f32 x, f32 y, f32 z) { GXPC_Put3F32(x, y, z); }
static inline void GXParam4f32(f32 x, f32 y, f32 z, f32 w) { GXPC_Put4F32(x, y, z, w); }

static inline void GXPosition3f32(f32 x, f32 y, f32 z) { GXPC_Put3F32(x, y, z); }
static inline void GXPosition3u8(u8 x, u8 y, u8 z) { GXPC_Put3U8(x, y, z); }
static inline void GXPosition3s8(s8 x, s8 y, s8 z) { GXPC_Put3U8((uint8_t)x, (uint8_t)y, (uint8_t)z); }
static inline void GXPosition3u16(u16 x, u16 y, u16 z) { GXPC_Put3U16(x, y, z); }
static inline void GXPosition3s16(s16 x, s16 y, s16 z) { GXPC_Put3U16((uint16_t)x, (uint16_t)y, (uint16_t)z); }
static inline void GXPosition2f32(f32 x, f32 y) { GXPC_Put2F32(x, y); }
static inline void GXPosition2u8(u8 x, u8 y) { GXPC_Put2U8(x, y); }
static inline void GXPosition2s8(s8 x, s8 y) { GXPC_Put2U8((uint8_t)x, (uint8_t)y); }
static inline void GXPosition2u16(u16 x, u16 y) { GXPC_Put2U16(x, y); }
static inline void GXPosition2s16(s16 x, s16 y) { GXPC_Put2U16((uint16_t)x, (uint16_t)y); }
static inline void GXPosition1x16(u16 x) { GXPC_Put16(x); }
static inline void GXPosition1x8(u8 x) { GXPC_Put8(x); }

static inline void GXNormal3f32(f32 x, f32 y, f32 z) { GXPC_Put3F32(x, y, z); }
static inline void GXNormal3s16(s16 x, s16 y, s16 z) { GXPC_Put3U16((uint16_t)x, (uint16_t)y, (uint16_t)z); }
static inline void GXNormal3s8(s8 x, s8 y, s8 z) { GXPC_Put3U8((uint8_t)x, (uint8_t)y, (uint8_t)z); }
static inline void GXNormal1x16(u16 x) { GXPC_Put16(x); }
static inline void GXNormal1x8(u8 x) { GXPC_Put8(x); }

static inline void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { GXPC_Put4U8(r, g, b, a); }
static inline void GXColor1u32(u32 x) { GXPC_Put32((uint32_t)x); }
static inline void GXColor3u8(u8 r, u8 g, u8 b) { GXPC_Put3U8(r, g, b); }
static inline void GXColor1u16(u16 x) { GXPC_Put16(x); }
static inline void GXColor1x16(u16 x) { GXPC_Put16(x); }
static inline void GXColor1x8(u8 x) { GXPC_Put8(x); }

static inline void GXTexCoord2f32(f32 s, f32 t) { GXPC_Put2F32(s, t); }
static inline void GXTexCoord2s16(s16 s, s16 t) { GXPC_Put2U16((uint16_t)s, (uint16_t)t); }
static inline void GXTexCoord2u16(u16 s, u16 t) { GXPC_Put2U16(s, t); }
static inline void GXTexCoord2s8(s8 s, s8 t) { GXPC_Put2U8((uint8_t)s, (uint8_t)t); }
static inline void GXTexCoord2u8(u8 s, u8 t) { GXPC_Put2U8(s, t); }
static inline void GXTexCoord1f32(f32 s) { GXPC_PutF32(s); }
static inline void GXTexCoord1s16(s16 s) { GXPC_Put16((uint16_t)s); }
static inline void GXTexCoord1u16(u16 s) { GXPC_Put16(s); }
static inline void GXTexCoord1s8(s8 s) { GXPC_Put8((uint8_t)s); }
static inline void GXTexCoord1u8(u8 s) { GXPC_Put8(s); }
static inline void GXTexCoord1x16(u16 x) { GXPC_Put16(x); }
static inline void GXTexCoord1x8(u8 x) { GXPC_Put8(x); }
static inline void GXMatrixIndex1u8(u8 x) { GXPC_Put8(x); }

#ifdef __cplusplus
}
#endif

#ifndef _DOLPHIN_GX_GXVERT_H_
#define _DOLPHIN_GX_GXVERT_H_
#endif

#endif
