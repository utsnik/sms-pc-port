#include "runtime.h"

#include "gx_internal.h"
#include "port/port.h"
#include "sms_gx/gx_pc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

bool s_logged;
alignas(64) uint8_t s_fifo[64 * 1024];

void note_stub()
{
    if (s_logged)
        return;
    s_logged = true;
    port_log("SMG Wii U: GX link stub active");
}

} // namespace

extern "C" uint8_t* __PortGXFifoPtr = s_fifo;
extern "C" uint8_t* __PortGXFifoLimit = s_fifo + sizeof(s_fifo) - 64;
extern "C" unsigned char GXNtsc480Int[0x40] = {};

extern "C" void __PortGXFifoOverflow(void)
{
    note_stub();
    __PortGXFifoPtr = s_fifo;
}

extern "C" void smg_wiiu_gx_link_stub_init(void)
{
    note_stub();
}

/* Sunshine's GX API, FIFO parser, and vertex loader remain the sole owners of
 * the game-facing command stream. These are only their absent renderer-side
 * endpoints for the Stage 1 link boundary. */
namespace gx {

double g_flushSeconds;
uint32_t g_traceLastVat;
uint32_t g_displayFrames;
bool g_gxStats;

bool rendererReady() { return false; }
void flushBatch() {}
void onStateChange() {}
void executeCopy(uint32_t) { note_stub(); }
void markXfMemDirty() {}
/* The vertex loader decodes each primitive into the buffer this returns, then
 * the renderer would consume it. Without a renderer the bytes are discarded,
 * but the buffer must be writable: nullptr made the first GXBegin
 * (MainLoopFramework::clearEfb) fault in gx::readVec. */
uint8_t* primitiveBegin(uint8_t, uint32_t count, uint32_t, uint32_t stride)
{
    static uint8_t* s_sink;
    static size_t s_sink_bytes;
    const size_t need = size_t(count) * stride;
    if (need > s_sink_bytes) {
        uint8_t* grown = static_cast<uint8_t*>(realloc(s_sink, need));
        if (!grown)
            return s_sink;
        s_sink = grown;
        s_sink_bytes = need;
    }
    return s_sink;
}
void primitiveEnd(uint8_t, uint32_t) {}
PrimClass primClass(uint8_t opcode)
{
    return opcode >= 0xb8 ? PRIM_POINTS : opcode >= 0xa8 ? PRIM_LINES : PRIM_TRIS;
}
uint32_t primitiveIndices(uint8_t, uint32_t, uint32_t, uint32_t*) { return 0; }
void appendDecoded(PrimClass, uint32_t, uint32_t, const uint8_t*, uint32_t,
                   const uint32_t*, uint32_t) {}
void appendArena(PrimClass, uint32_t, uint32_t, const ArenaRef&, uint32_t,
                 uint32_t, const uint8_t*) {}
bool arenaEnabled() { return false; }
uint32_t arenaGeneration() { return 0; }
bool arenaUpload(const uint8_t*, uint32_t, uint32_t, const uint32_t*, uint32_t,
                 ArenaRef&) { return false; }
bool batchUsesArena() { return false; }
uint32_t peekColor(int, int) { return 0; }
uint32_t peekZ(int, int) { return 0x00ffffffu; }
void pixMetricClear() {}
uint32_t pixMetricRead() { return 0; }
void textureInvalidateAll() {}
FILE* traceFile() { return nullptr; }
void traceNoteBegin() {}
void traceNoteProjection(const float*) {}

} // namespace gx

extern "C" int GXPC_InitAuto(float)
{
    note_stub();
    return 0;
}

extern "C" void GXSetZScaleOffset(float, float) { note_stub(); }
extern "C" void port_gx_marker(unsigned int) {}
extern "C" void port_gx_camera(const float*) {}
