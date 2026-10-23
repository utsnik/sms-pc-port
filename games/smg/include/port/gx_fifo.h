// Replacement for the Broadway write-gather pipe (0xCC008000).
//
// The decompiled GX library builds exactly the command stream the real GPU would
// receive.  On the port, every FIFO write goes to the shared GX command pipe
// (sms_gx's GXPC_Write*, big-endian like the hardware stream and like display
// lists in game data), the same stream GXBegin and the rest of the GX API write.
// GXBegin writes its header there, so the vertices written after it with
// GXVert.h's inline writers, and the J3D FIFO loads (J3DFifo.hpp), must land in
// the same stream: a separate buffer left every immediate-mode draw (layouts,
// EFB clears) decoding the following commands as its vertex data.
#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sms_gx's pipe (platform/gx/include/sms_gx/gx_pc.h). */
void GXPC_Write8(uint8_t v);
void GXPC_Write16(uint16_t v);
void GXPC_Write32(uint32_t v);
extern uint8_t* GXPC_PipeBuf;
extern uint32_t GXPC_PipeLen, GXPC_PipeFast;

static inline void __PortGXFifo_u8(uint8_t v) {
    if (GXPC_PipeLen + 1 <= GXPC_PipeFast) GXPC_PipeBuf[GXPC_PipeLen++] = v;
    else GXPC_Write8(v);
}
static inline void __PortGXFifo_s8(int8_t v) { __PortGXFifo_u8((uint8_t)v); }

static inline void __PortGXFifo_u16(uint16_t v) {
    if (GXPC_PipeLen + 2 <= GXPC_PipeFast) {
        uint8_t* p = GXPC_PipeBuf + GXPC_PipeLen;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
        GXPC_PipeLen += 2;
    } else {
        GXPC_Write16(v);
    }
}
static inline void __PortGXFifo_s16(int16_t v) { __PortGXFifo_u16((uint16_t)v); }

/* Values are host integers; the pipe stores them big-endian (byte by byte),
 * so no swap is needed here on either host byte order. */
static inline void __PortGXFifo_u32(uint32_t v) {
    if (GXPC_PipeLen + 4 <= GXPC_PipeFast) {
        uint8_t* p = GXPC_PipeBuf + GXPC_PipeLen;
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
        GXPC_PipeLen += 4;
    } else {
        GXPC_Write32(v);
    }
}
static inline void __PortGXFifo_s32(int32_t v) { __PortGXFifo_u32((uint32_t)v); }

static inline void __PortGXFifo_f32(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    __PortGXFifo_u32(v);
}

static inline void __PortGXFifo_u64(uint64_t v) {
    __PortGXFifo_u32((uint32_t)(v >> 32));
    __PortGXFifo_u32((uint32_t)v);
}
static inline void __PortGXFifo_s64(int64_t v) { __PortGXFifo_u64((uint64_t)v); }
static inline void __PortGXFifo_f64(double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    __PortGXFifo_u64(v);
}

#ifdef __cplusplus
}
#endif
