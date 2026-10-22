#!/usr/bin/env python3
"""Verify the Task 1 Petari source ownership boundary."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "games/smg/cmake/sources.cmake"
SOURCE_ROOT = ROOT / "games/smg/decomp/src"

EXCLUDED_FAMILIES = (
    "gx", "os", "mem", "ai", "dsp", "ax", "axfx", "vi", "dvd",
    "pad", "kpad", "wpad",
)
APPROVED_SDK = {
    "RVL_SDK/arc/arc.c",
    "RVL_SDK/aralt/aralt.c",
    "RVL_SDK/tpl/TPL.c",
    "RVL_SDK/gd/GDIndirect.c",
    "RVL_SDK/gd/GDTexture.c",
}
DUPLICATE_GD = {
    "GDBase.c", "GDGeometry.c", "GDLight.c", "GDPixel.c",
    "GDTev.c", "GDTransform.c",
}


def fail(message: str) -> int:
    print(f"source-boundary: FAIL: {message}")
    return 1


def main() -> int:
    if not MANIFEST.exists():
        return fail("manifest not implemented")

    text = MANIFEST.read_text(encoding="utf-8")
    required = (
        'src/Game/*.cpp', 'src/Game/*.c', 'src/JSystem/*.cpp',
        'src/nw4r/*.cpp',
        'RVL_SDK/arc/arc.c', 'RVL_SDK/aralt/aralt.c',
        'RVL_SDK/tpl/TPL.c', 'RVL_SDK/gd/GDIndirect.c',
        'RVL_SDK/gd/GDTexture.c',
    )
    missing = [item for item in required if item not in text]
    if missing:
        return fail("manifest missing: " + ", ".join(missing))

    if re.search(r'GLOB_RECURSE[^\n]*RVL_SDK', text):
        return fail("RVL_SDK must not be selected by a broad glob")

    if not SOURCE_ROOT.exists():
        return fail("copied source tree is absent")

    actual = {
        path.relative_to(SOURCE_ROOT).as_posix()
        for path in SOURCE_ROOT.rglob("*")
        if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".S"}
    }
    allowed = {
        path for path in actual
        if path.startswith("Game/")
        or path.startswith("JSystem/")
        or path.startswith("nw4r/")
        or path in APPROVED_SDK
    }
    unexpected = sorted(actual - allowed)
    if unexpected:
        return fail("unapproved Petari sources copied: " + ", ".join(unexpected[:8]))

    selected_paths = set(re.findall(r'RVL_SDK/[^" )]+\.(?:c|cpp)', text))
    if selected_paths != APPROVED_SDK:
        return fail("approved Petari SDK set is not exact")

    for family in EXCLUDED_FAMILIES:
        if any(path.startswith(f"RVL_SDK/{family}/") for path in actual):
            return fail(f"excluded Petari SDK family present: {family}")
        if re.search(rf"RVL_SDK/{re.escape(family)}/", text):
            return fail(f"excluded Petari SDK family selected: {family}")

    for duplicate in DUPLICATE_GD:
        if f"RVL_SDK/gd/{duplicate}" in text:
            return fail(f"duplicate Petari GD source selected: {duplicate}")

    print("source-boundary: PASS: Game/JSystem/NW4R plus approved Petari SDK files only")
    print("source-boundary: excluded Petari SDK families: " + ", ".join(EXCLUDED_FAMILIES))
    print("source-boundary: duplicate Petari GD files: GDBase.c, GDGeometry.c, GDLight.c, GDPixel.c, GDTev.c, GDTransform.c")
    return 0


if __name__ == "__main__":
    sys.exit(main())
