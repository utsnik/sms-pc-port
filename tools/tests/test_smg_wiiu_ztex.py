#!/usr/bin/env python3
"""Host regression tests for the GX Z-texture expansion and shader wiring."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "platform/wiiu/gx2_renderer.cpp"


def main() -> int:
    source = r'''
#include "platform/wiiu/gx2_ztex.h"
#include <cmath>
int main() {
    using gx::gx2ZTextureValue;
    using gx::gx2ZTextureAddValue;
    if (std::fabs(gx2ZTextureValue(1, 0, 0, 0, 0, 0) - 255.0f / 16777215.0f) > 1e-8f) return 1;
    if (std::fabs(gx2ZTextureValue(0xcd / 255.0f, 0, 0, 0xab / 255.0f, 1, 0) - 0xabcd / 16777215.0f) > 1e-8f) return 2;
    if (std::fabs(gx2ZTextureValue(1, 1, 1, 0, 2, 0) - 1.0f) > 1e-8f) return 3;
    if (std::fabs(gx2ZTextureValue(1, 1, 1, 0, 2, 1) - 0.0f) > 1e-8f) return 4;
    if (std::fabs(gx2ZTextureAddValue(0.5f, 1, 1, 1, 0, 2, 0) - 0x7ffffe / 16777215.0f) > 1e-7f) return 5;
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / "ztex.cpp"
        exe = Path(tmp) / "ztex"
        cpp.write_text(source, encoding="utf-8")
        result = subprocess.run(["c++", "-std=c++17", "-I", str(ROOT), str(cpp), "-o", str(exe)], check=False)
        if result.returncode:
            print("ztex: FAIL: host compile")
            return result.returncode
        result = subprocess.run([str(exe)], check=False)
        if result.returncode:
            print(f"ztex: FAIL: host case {result.returncode}")
            return result.returncode
    source = RENDERER.read_text(encoding="utf-8")
    key = source[source.index("struct TevKey"):source.index("struct TevShader")]
    if "ztexBias" in key:
        print("ztex: FAIL: bias remains in shader key")
        return 6
    if "k.ztex2 = g.bp[BP_ZTEX2] & 0xf;" in source:
        print("ztex: FAIL: disabled z-texture state is not excluded from key")
        return 7
    if "const bool ztexAdd" not in source or "ra.pos" not in source:
        print("ztex: FAIL: GX_ZT_ADD rasterized depth path missing")
        return 8
    if "ra.scoord[stage]" not in source or "if (ztexStage == int(stage))" not in source:
        print("ztex: FAIL: z-texture does not reuse the indirect stage coordinate")
        return 9
    if "shader.key.ztexBias" in source or "16777215.0f" not in source:
        print("ztex: FAIL: z-texture bias/24-bit shader uniform wiring missing")
        return 10
    zpath_start = source.index("if (ztexExport) {")
    zpath_end = source.index("const size_t compareCount", zpath_start)
    zpath = source[zpath_start:zpath_end]
    required_emits = {
        "bias": "tevC(kZtexConstReg, kZtexBiasChan)",
        "raster range": "tevC(kZtexConstReg, kZtexRangeChan)",
        "normalisation range": "tevC(kZtexConstReg, kZtexRangeInvChan)",
    }
    for name, emit in required_emits.items():
        if emit not in zpath:
            print(f"ztex: FAIL: generated z path does not read named {name} constant")
            return 11
    if "tevC(41, _y)" in zpath or "tevC(41, _x)" in zpath or "tevC(42, _y)" in zpath:
        print("ztex: FAIL: generated z path still reads the old C41/C42 channels")
        return 12
    if "kZtexConstReg = 42" not in source or "kZtexRangeInvChan = _y" not in source:
        print("ztex: FAIL: z constant register/channel names are not pinned")
        return 13
    upload = source[source.index("params[156]"):source.index("// C43-C46", source.index("params[156]"))]
    for name in ("kZtexRangeInvChan", "kZtexRangeChan", "kZtexBiasChan"):
        if f"4 * kZtexConstReg + {name}" not in upload:
            print(f"ztex: FAIL: upload does not fill named {name} slot")
            return 14
    if "const bool ztexReplace" not in source or "if (ztexAdd)" not in zpath:
        print("ztex: FAIL: REPLACE/ADD z paths are not both covered")
        return 15
    print("ztex: PASS: ADD, 24-bit wrap/normalisation, stage coordinates, shader key")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
