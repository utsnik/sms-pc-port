#!/usr/bin/env python3
"""Check the finite Stage 1 shared-provider and stub boundary."""

from pathlib import Path
import argparse
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
CMK = ROOT / "platform/wiiu/CMakeLists.txt"
STUB_H = ROOT / "games/smg/platform/wiiu/sdk_stubs.h"
STUB_CPP = ROOT / "games/smg/platform/wiiu/sdk_stubs.cpp"

SHARED = (
    "platform/gx/src/gx_api.cpp", "platform/gx/src/gx_fifo.cpp",
    "platform/gx/src/gx_vert.cpp", "platform/wiiu/os_core.cpp",
    "platform/wiiu/os_thread.cpp", "platform/wiiu/runtime.cpp",
    "platform/wiiu/vi.cpp", "platform/wiiu/dvd.cpp", "platform/wiiu/pad.cpp",
    "platform/audio/ai.cpp", "platform/audio/dsp_hle.cpp",
    "platform/audio/dsp_mixer.cpp", "platform/audio/noaudio.cpp",
    "platform/ar/ar.cpp", "platform/mtx/mtx.cpp",
)


def fail(message: str) -> int:
    print(f"symbol-ownership: FAIL: {message}")
    return 1


def nm_lines(elf: Path):
    for tool in ("powerpc-eabi-nm", "nm"):
        try:
            result = subprocess.run(
                [tool, "-C", "--undefined-only", str(elf)],
                check=False, text=True, capture_output=True,
            )
        except FileNotFoundError:
            continue
        if result.returncode == 0:
            return [line for line in result.stdout.splitlines() if line.strip()]
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default="games/smg/platform/wiiu/symbol_ownership.md")
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    manifest = (ROOT / args.manifest).resolve()
    if not manifest.exists() or not CMK.exists() or not STUB_H.exists() or not STUB_CPP.exists():
        return fail("provider set is incomplete")
    text = manifest.read_text(encoding="utf-8")
    cmake = CMK.read_text(encoding="utf-8")
    smg_cmake = cmake.split("endif()", 1)[0]
    header = STUB_H.read_text(encoding="utf-8")
    source = STUB_CPP.read_text(encoding="utf-8")
    if "## Task 3 provider set" not in text:
        return fail("provider set is incomplete")
    if "SMG_WIIU_SHARED_PROVIDER_SOURCES" not in smg_cmake:
        return fail("shared provider source set is not declared")
    missing = [path for path in SHARED if smg_cmake.count(path) != 1]
    if missing:
        return fail("shared provider selection is not exactly once: " + ", ".join(missing))
    if "SMG_STAGE1_STUB_SYMBOLS" not in header or "SMG_STAGE1_STUB_SYMBOLS(SMG_DEFINE_STAGE1_STUB)" not in source:
        return fail("unassigned Galaxy stub set is incomplete")
    if "__attribute__((weak))" in source or "#pragma weak" in source or re.search(r"weak.*\*", source, re.I):
        return fail("blanket weak stub is forbidden")
    stubs = re.findall(r"\n\s*X\(([A-Za-z_][A-Za-z0-9_]*)\)", header)
    if not stubs or len(stubs) != len(set(stubs)):
        return fail("stub list is not finite and unique")
    if args.elf:
        undefined = nm_lines(args.elf)
        if undefined is None:
            return fail("unable to inspect ELF undefined symbols")
        if undefined:
            return fail("ELF has undefined symbols: " + "; ".join(undefined[:6]))
    print("symbol-ownership: PASS: strong providers=0 duplicates; stage1-stubs=" + str(len(stubs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
