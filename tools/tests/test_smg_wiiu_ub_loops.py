#!/usr/bin/env python3
"""Galaxy's out-of-bounds retail loops must keep their exits on Wii U.

CameraShaker::createSinglyHorizontalTask runs i < 7 over a 3-entry array (a
Nintendo bug that CodeWarrior compiled literally). GCC's aggressive loop
optimizations used the array bound to drop the exit test, so the loop allocated
CameraShake tasks until the FileSelect scene heap was exhausted (OSPanic in
GameSystemSceneController::initializeScene). The SMG game flags must carry
-fno-aggressive-loop-optimizations, and the linked function must test its
counter (a conditional backward branch).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
FUNC = "_ZN12CameraShaker26createSinglyHorizontalTaskEv"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    if "-fno-aggressive-loop-optimizations" not in args.cmake.read_text(encoding="utf-8"):
        print("ub-loops: FAIL: SMG game flags lack -fno-aggressive-loop-optimizations")
        return 1
    if args.elf:
        elf = args.elf.resolve()
        text = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
             BUILDER_IMAGE, OBJDUMP, "-d", f"/e/{elf.name}"],
            check=True, capture_output=True, text=True).stdout
        start = text.find(f"<{FUNC}>:\n")
        if start < 0:
            print("ub-loops: FAIL: createSinglyHorizontalTask not found")
            return 1
        body = text[start:text.find("\n\n", start)]
        # A conditional branch back to an earlier address is the loop's exit
        # test; the miscompiled loop only had an unconditional "b" back.
        back = False
        for m in re.finditer(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(b(?:ne|eq|lt|gt|le|ge|dnz)[-+]?)\s+([0-9a-f]+)",
                             body, re.M):
            if int(m.group(3), 16) < int(m.group(1), 16):
                back = True
        if not back:
            print("ub-loops: FAIL: createSinglyHorizontalTask has no loop exit (endless allocation)")
            return 1
    print("ub-loops: PASS: retail out-of-bounds loops keep their exit tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
