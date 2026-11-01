#!/usr/bin/env python3
"""Host regression for the marker-gated normal-Bloom copy-size selection."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "imgfx_copy.h"

using sms_wiiu_imgfx::CopySizes;
using sms_wiiu_imgfx::kCopyOwnerBloom;
using sms_wiiu_imgfx::kCopyOwnerUntagged;
using sms_wiiu_imgfx::selectCopySizes;

static int check(const CopySizes& s, unsigned logicalW, unsigned logicalH,
                 unsigned sourceW, unsigned sourceH, unsigned physicalW, unsigned physicalH) {
    return s.native && s.logicalW == logicalW && s.logicalH == logicalH &&
           s.sourceW == sourceW && s.sourceH == sourceH &&
           s.physicalW == physicalW && s.physicalH == physicalH;
}

int main() {
    const unsigned sizes[][2] = {{640, 456}, {320, 228}, {160, 114}, {80, 57}};
    for (const auto& size : sizes) {
        CopySizes selected = selectCopySizes(true, kCopyOwnerBloom, size[0], size[1], false,
                                             size[0], size[1], 2.0f, 1.5f, false, 1280, 720);
        if (!check(selected, size[0], size[1], size[0], size[1], size[0], size[1])) return 1;
    }
    CopySizes mipmapped = selectCopySizes(true, kCopyOwnerBloom, 640, 456, true,
                                          320, 228, 2.0f, 1.5f, false, 1280, 720);
    if (!check(mipmapped, 320, 228, 640, 456, 320, 228)) return 2;

    CopySizes untagged = selectCopySizes(true, kCopyOwnerUntagged, 640, 456, false,
                                         320, 228, 2.0f, 1.5f, false, 1280, 720);
    if (untagged.native || untagged.logicalW != 640 || untagged.logicalH != 456 ||
        untagged.sourceW != 1280 || untagged.sourceH != 684 ||
        untagged.physicalW != 1280 || untagged.physicalH != 684) return 3;
    CopySizes markerOff = selectCopySizes(false, kCopyOwnerBloom, 640, 456, false,
                                          320, 228, 2.0f, 1.5f, false, 1280, 720);
    if (markerOff.native || markerOff.sourceW != untagged.sourceW ||
        markerOff.sourceH != untagged.sourceH || markerOff.physicalW != untagged.physicalW ||
        markerOff.physicalH != untagged.physicalH) return 4;
    return 0;
}
'''


def main() -> int:
    renderer = (ROOT / "platform/wiiu/gx2_renderer.cpp").read_text(encoding="utf-8")
    bloom = (ROOT / "games/smg/decomp/src/Game/Screen/BloomEffect.cpp").read_text(encoding="utf-8")
    required = (
        's_imgfxNative = sdMarkerPresent("imgfxnative")',
        'sms_wiiu_imgfx::selectCopySizes',
        'static bool s_imgfxNative',
        '[copyown] owner=%s n=%u logical=%ux%u physical=%ux%u',
    )
    missing = [needle for needle in required if needle not in renderer]
    if missing:
        print("smg-wiiu-imgfxnative: FAIL: renderer wiring missing " + repr(missing))
        return 1
    if bloom.count("ImageEffectLocalUtil::CaptureOwner_NormalBloom") < 9:
        print("smg-wiiu-imgfxnative: FAIL: BloomEffect capture owner tags missing")
        return 1

    with tempfile.TemporaryDirectory(prefix="smg-imgfxnative-") as directory:
        src = Path(directory) / "imgfxnative.cpp"
        exe = Path(directory) / "imgfxnative"
        src.write_text(HARNESS, encoding="utf-8")
        build = subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "platform/wiiu"),
             str(src), "-o", str(exe)], capture_output=True, text=True,
        )
        if build.returncode:
            print("smg-wiiu-imgfxnative: FAIL: host compile failed: " + build.stderr.strip())
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode:
            print(f"smg-wiiu-imgfxnative: FAIL: case {run.returncode}")
            return 1
    print("smg-wiiu-imgfxnative: PASS: Bloom full/half/quarter/eighth, mipmapped, and untagged copies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
