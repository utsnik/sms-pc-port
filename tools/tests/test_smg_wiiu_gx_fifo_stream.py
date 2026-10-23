#!/usr/bin/env python3
"""Galaxy's FIFO writers must feed the same GX command stream as GXBegin.

GXBegin (sms_gx's gx_api.cpp) writes its primitive header to the shared GX
pipe through GXPC_Write*. The vertices that follow it are written by
GXVert.h's inline writers (GXPosition3f32, GXColor1u32, GXTexCoord2f32, ...),
and J3D's FIFO loads (J3DFifo.hpp: GXCmd1u8/GXParam1u32) the same way, all
through games/smg/include/port/gx_fifo.h. When gx_fifo.h wrote into a
separate buffer that nothing parsed, every immediate-mode draw (every
nw4r::lyt pane, the EFB clear quads) decoded the commands after its header as
vertex data: no title logo, no file-select items.

This host test compiles gx_fifo.h against a fake pipe that records both the
inline fast path and the GXPC_Write* calls, emits a GXBegin header with the
pipe's own calls followed by one vertex through the FIFO writers, and checks
the bytes arrive in order, big-endian, in the one stream. It also checks that
GXVert.h and GXRegs.h route their writes through gx_fifo.h.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIFO_INCLUDE = ROOT / "games/smg/include"
GXVERT = ROOT / "games/smg/decomp/libs/RVL_SDK/include/revolution/gx/GXVert.h"
GXREGS = ROOT / "games/smg/decomp/libs/RVL_SDK/include/revolution/gx/GXRegs.h"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "port/gx_fifo.h"

static uint8_t stream[256];
static uint32_t streamLen;
static uint8_t fast[256];
uint8_t* GXPC_PipeBuf = fast;
uint32_t GXPC_PipeLen = 0, GXPC_PipeFast = 0;

/* The real pipe appends both paths to one buffer; mirror that. */
static void sync(void) {
    memcpy(stream + streamLen, fast, GXPC_PipeLen);
    streamLen += GXPC_PipeLen;
    GXPC_PipeLen = 0;
}
void GXPC_Write8(uint8_t v) { sync(); stream[streamLen++] = v; }
void GXPC_Write16(uint16_t v) { sync(); stream[streamLen++] = v >> 8; stream[streamLen++] = (uint8_t)v; }
void GXPC_Write32(uint32_t v) {
    sync();
    stream[streamLen++] = v >> 24; stream[streamLen++] = v >> 16;
    stream[streamLen++] = v >> 8; stream[streamLen++] = (uint8_t)v;
}

int main(void) {
    static const uint8_t want[] = {
        0x80, 0x00, 0x01,             /* GXBegin(GX_QUADS, GX_VTXFMT0, 1) */
        0x3f, 0x80, 0x00, 0x00,       /* GXPosition3f32(1, -2, 0.5) */
        0xc0, 0x00, 0x00, 0x00,
        0x3f, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33, 0x44,       /* GXColor1u32 */
        0x02, 0x80,                   /* GXPosition2u16-style u16 */
        0x7f,                         /* u8 */
    };
    for (int fastCap = 0; fastCap <= 64; fastCap += 64) {
        streamLen = 0;
        GXPC_PipeLen = 0;
        GXPC_PipeFast = (uint32_t)fastCap;
        GXPC_Write8(0x80);
        GXPC_Write16(1);
        __PortGXFifo_f32(1.0f);
        __PortGXFifo_f32(-2.0f);
        __PortGXFifo_f32(0.5f);
        __PortGXFifo_u32(0x11223344u);
        __PortGXFifo_u16(0x0280);
        __PortGXFifo_u8(0x7f);
        sync();
        if (streamLen != sizeof(want) || memcmp(stream, want, sizeof(want)) != 0) {
            printf("stream mismatch (fast=%d, %u bytes):", fastCap, streamLen);
            for (uint32_t i = 0; i < streamLen; ++i) printf(" %02x", stream[i]);
            printf("\n");
            return 1;
        }
    }
    return 0;
}
"""


def fail(message: str) -> int:
    print(f"gx-fifo-stream: FAIL: {message}")
    return 1


def main() -> int:
    vert = GXVERT.read_text(encoding="utf-8")
    regs = GXREGS.read_text(encoding="utf-8")
    if '#include "port/gx_fifo.h"' not in vert or "__PortGXFifo_##td" not in vert:
        return fail("GXVert.h does not write through port/gx_fifo.h")
    if "__PortGXFifo_u32((u32)(ui))" not in regs:
        return fail("GXRegs.h GX_WRITE_* does not write through port/gx_fifo.h")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "harness.c"
        exe = Path(tmp) / "harness"
        src.write_text(HARNESS, encoding="utf-8")
        build = subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", "-I", str(FIFO_INCLUDE),
                                str(src), "-o", str(exe)], capture_output=True, text=True)
        if build.returncode != 0:
            return fail("gx_fifo.h does not build against the GX pipe (GXPC_Write*, GXPC_PipeBuf): "
                        + build.stderr.strip().splitlines()[-1] if build.stderr.strip() else "build failed")
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode != 0:
            return fail(run.stdout.strip() or "harness failed")
    print("gx-fifo-stream: PASS: FIFO writers append to GXBegin's stream, in order, big-endian")
    return 0


if __name__ == "__main__":
    sys.exit(main())
