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
    print("ztex: PASS: ADD, 24-bit wrap/normalisation, stage coordinates, shader key")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
