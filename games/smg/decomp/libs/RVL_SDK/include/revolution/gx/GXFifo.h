#ifndef GXFIFO_H
#define GXFIFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/types.h"

typedef void (*GXBreakPtCallback)(void);

typedef struct _GXFifoObj {
    u8 pad[128];
} GXFifoObj;

typedef struct __GXFifoObj {
    u8* base;
    u8* top;
    u32 size;
    u32 hiWatermark;
    u32 loWatermark;
    void* rdPtr;
    void* wrPtr;
    s32 count;
    GXBool wrap;
    GXBool bind_cpu;
    GXBool bind_gp;
} __GXFifoObj;

#if defined(__WIIU__)
/* SMG's RVL declaration is an out-parameter API, while the Wii U provider
 * exposes Dolphin's return-pointer form under the same C symbol. */
GXFifoObj* GXGetCPUFifo(void);
GXFifoObj* GXGetGPFifo(void);
static inline GXBool GXGetCPUFifoOut(GXFifoObj* fifo) {
    *fifo = *GXGetCPUFifo();
    return GX_TRUE;
}
static inline GXBool GXGetGPFifoOut(GXFifoObj* fifo) {
    *fifo = *GXGetGPFifo();
    return GX_TRUE;
}
#else
GXBool GXGetCPUFifo(GXFifoObj*);
GXBool GXGetGPFifo(GXFifoObj*);
#endif

void GXGetFifoPtrs(const GXFifoObj*, void**, void**);

void GXEnableBreakPt(void* pBreakPoint);
void GXDisableBreakPt(void);

GXBool __GXIsGPFifoReady(void);

void __GXFifoInit(void);
void GXInitFifoBase(GXFifoObj*, void*, u32);
void GXSetCPUFifo(const GXFifoObj*);
void GXSetGPFifo(const GXFifoObj*);

GXBool GXGetFifoWrap(const GXFifoObj* fifo);

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback);
void GXGetGPStatus(GXBool*, GXBool*, GXBool*, GXBool*, GXBool*);

#ifdef __cplusplus
}
#endif

#endif  // GXFIFO_H
