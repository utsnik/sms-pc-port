#!/usr/bin/env python3
"""Host regression for the faststate register dependency split."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "gx_internal.h"
using namespace gx;

static unsigned generation;
static void xf(unsigned addr, unsigned oldValue, unsigned newValue) {
    if (oldValue != newValue && fastStateXfWriteAffectsKeys((uint16_t)addr)) ++generation;
}
static void bp(unsigned reg, unsigned oldValue, unsigned newValue) {
    if (oldValue != newValue && fastStateBpWriteAffectsKeys(reg)) ++generation;
}
int main() {
    xf(XF_MEM_WORDS - 1, 0, 1);                 // matrix/light memory
    if (generation != 0) return 1;
    bp(BP_TEV_REG, 0, 1);                       // TEV colour/konst storage
    if (generation != 0) return 2;
    bp(BP_TEV_COLOR_ENV, 0, 1);                 // stage selector consumed by TevKey
    if (generation != 1) return 3;
    bp(BP_TEV_COLOR_ENV, 1, 1);                 // unchanged masked value
    if (generation != 1) return 4;
    bp(0xDF, 0, 1);                             // stage-15 colour env
    if (generation != 2) return 5;
    bp(0x2F, 0, 1);                             // stage-15 TREF
    if (generation != 3) return 6;
    bp(0x1F, 0, 1);                             // stage-15 indirect command
    if (generation != 4) return 7;
    bp(0xFD, 0, 1);                             // upper-stage KSEL
    if (generation != 5) return 8;
    return 0;
}
'''


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "faststate.cpp"
        exe = Path(tmp) / "faststate"
        src.write_text(HARNESS, encoding="utf-8")
        build = subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Werror", "-I", str(ROOT / "platform/gx/src"),
             str(src), "-o", str(exe)], capture_output=True, text=True)
        if build.returncode:
            print("smg-wiiu-faststate-invalidation: FAIL: " + build.stderr.strip())
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode:
            print(f"smg-wiiu-faststate-invalidation: FAIL: case {run.returncode}")
            return 1
    print("smg-wiiu-faststate-invalidation: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
