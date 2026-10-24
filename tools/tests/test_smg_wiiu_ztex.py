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
#include <cstdint>

static float eval_table(float r, float g, float b, float a, float raster,
                        uint32_t format, uint32_t bias, bool add) {
    const gx::Gx2ZTextureOpTable table = gx::gx2ZTextureOpTable(format, add);
    float values[2] = {};
    const float tex[4] = {r, g, b, a};
    for (uint32_t i = 0; i < table.count; ++i) {
        const gx::Gx2ZTextureOp& op = table.ops[i];
        volatile float lhs;
        volatile float rhs;
        volatile float result;
        switch (op.lhs) {
        case gx::kGx2ZTexR: lhs = tex[0]; break;
        case gx::kGx2ZTexG: lhs = tex[1]; break;
        case gx::kGx2ZTexB: lhs = tex[2]; break;
        case gx::kGx2ZTexA: lhs = tex[3]; break;
        case gx::kGx2ZTexZ: lhs = values[0]; break;
        case gx::kGx2ZTexQ: lhs = values[1]; break;
        case gx::kGx2ZTexByteMax: lhs = 255.0f; break;
        case gx::kGx2ZTexByteScale: lhs = 256.0f; break;
        case gx::kGx2ZTexWrapInv: lhs = 1.0f / 16777216.0f; break;
        case gx::kGx2ZTexWrapRange: lhs = 16777216.0f; break;
        case gx::kGx2ZTexRangeInv: lhs = 1.0f / 16777215.0f; break;
        case gx::kGx2ZTexRange: lhs = 16777215.0f; break;
        case gx::kGx2ZTexBias: lhs = float(bias & 0x00ffffffu); break;
        case gx::kGx2ZTexRaster: lhs = raster; break;
        default: return -100.0f;
        }
        switch (op.rhs) {
        case gx::kGx2ZTexR: rhs = tex[0]; break;
        case gx::kGx2ZTexG: rhs = tex[1]; break;
        case gx::kGx2ZTexB: rhs = tex[2]; break;
        case gx::kGx2ZTexA: rhs = tex[3]; break;
        case gx::kGx2ZTexZ: rhs = values[0]; break;
        case gx::kGx2ZTexQ: rhs = values[1]; break;
        case gx::kGx2ZTexByteMax: rhs = 255.0f; break;
        case gx::kGx2ZTexByteScale: rhs = 256.0f; break;
        case gx::kGx2ZTexWrapInv: rhs = 1.0f / 16777216.0f; break;
        case gx::kGx2ZTexWrapRange: rhs = 16777216.0f; break;
        case gx::kGx2ZTexRangeInv: rhs = 1.0f / 16777215.0f; break;
        case gx::kGx2ZTexRange: rhs = 16777215.0f; break;
        case gx::kGx2ZTexBias: rhs = float(bias & 0x00ffffffu); break;
        case gx::kGx2ZTexRaster: rhs = raster; break;
        default: return -100.0f;
        }
        if (op.negateRhs) rhs = -rhs;
        if (op.alu == gx::kGx2ZMul) result = lhs * rhs;
        else if (op.alu == gx::kGx2ZAdd) result = lhs + rhs;
        else result = floorf(lhs);
        values[op.dst == gx::kGx2ZTexQ ? 1 : 0] = result;
    }
    return values[0];
}

static bool close(float a, float b) {
    return std::fabs(a - b) <= 2.0e-7f;
}

int main() {
    using gx::gx2ZTextureValue;
    using gx::gx2ZTextureAddValue;
    struct Case { float r, g, b, a, raster; uint32_t format, bias; bool add; };
    const Case cases[] = {
        {1, 1, 1, 0, 0, 2, 0, false},             // Z24X8: 0xffffff
        {0x80 / 255.0f, 0, 0, 0, 0, 2, 0, false}, // Z24X8: 0x800000
        {0, 0, 0, 0, 0, 2, 0, false},             // Z24X8: 0
        {0xcd / 255.0f, 0, 0, 0xab / 255.0f, 0, 1, 0, false}, // Z16
        {0x7f / 255.0f, 0, 0, 0, 0, 0, 0, false},             // Z8
        {1, 1, 1, 0, 0, 2, 1, false},             // bias overflow
        {1, 1, 1, 0, 1, 2, 0, true},              // ADD with wrap
    };
    for (const Case& c : cases) {
        const float expected = c.add ? gx2ZTextureAddValue(c.raster, c.r, c.g, c.b, c.a, c.format, c.bias)
                                     : gx2ZTextureValue(c.r, c.g, c.b, c.a, c.format, c.bias);
        const float actual = eval_table(c.r, c.g, c.b, c.a, c.raster, c.format, c.bias, c.add);
        if (!close(actual, expected)) return 1;
    }
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
    if "tevC(41, _w)" not in zpath or "tevC(41, _z)" not in zpath:
        print("ztex: FAIL: generated z path does not use C41's 2^24 wrap constants")
        return 12
    if "kZtexConstReg = 47" not in source or "kZtexRangeInvChan = _y" not in source:
        print("ztex: FAIL: z constant register/channel names are not pinned")
        return 13
    upload = source[source.index("params[156]"):source.index("// C43-C46", source.index("params[156]"))]
    for name in ("kZtexRangeInvChan", "kZtexRangeChan", "kZtexBiasChan"):
        if f"4 * kZtexConstReg + {name}" not in upload:
            print(f"ztex: FAIL: upload does not fill named {name} slot")
            return 14
    if "kTevUniforms = 48" not in source:
        print("ztex: FAIL: C47 is not included in the pixel uniform allocation")
        return 15
    z_slots = {4 * 47 + 1, 4 * 47 + 2, 4 * 47 + 3}
    owners = {
        "tev-reg": set(range(0, 16)),
        "konst": set(range(16, 32)),
        "fixed": set(range(32, 60)),
        "indirect": set(range(60, 92)) | set(range(156, 172)),
        "texture": set(range(92, 156)),
        "low-byte": set(range(172, 188)),
    }
    for owner, slots in owners.items():
        if z_slots & slots:
            print(f"ztex: FAIL: z upload overlaps {owner} constant slots")
            return 16
    if "const bool ztexReplace" not in source or "gx2ZTextureOpTable(zfmt, ztexAdd)" not in zpath:
        print("ztex: FAIL: shared REPLACE/ADD z operation tables are not wired")
        return 17
    print("ztex: PASS: shared float32 ALU table, ADD, 24-bit wrap/normalisation, constant map")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
