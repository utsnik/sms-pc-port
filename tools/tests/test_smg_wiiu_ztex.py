#!/usr/bin/env python3
"""Host regression tests for the GX Z-texture byte expansion and bias."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    source = r'''
#include "platform/wiiu/gx2_ztex.h"
#include <cmath>
int main() {
    using gx::gx2ZTextureValue;
    if (std::fabs(gx2ZTextureValue(1, 0, 0, 0, 0, 0) - 255.0f / 16777216.0f) > 1e-8f) return 1;
    if (std::fabs(gx2ZTextureValue(0xcd / 255.0f, 0, 0, 0xab / 255.0f, 1, 0) - 0xabcd / 16777216.0f) > 1e-8f) return 2;
    if (std::fabs(gx2ZTextureValue(1, 1, 1, 0, 2, 0x123456) - (16777215.0f + 0x123456) / 16777216.0f) > 1e-7f) return 3;
    if (std::fabs(gx2ZTextureValue(0, 0, 0, 0, 2, 7) - 7.0f / 16777216.0f) > 1e-8f) return 4;
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
            print(f"ztex: FAIL: case {result.returncode}")
            return result.returncode
    print("ztex: PASS: Z8/Z16/Z24X8 decode and bias")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
