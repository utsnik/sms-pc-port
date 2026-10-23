#!/usr/bin/env python3
"""Galaxy's out-of-bounds retail loops must be bounded on Wii U.

CameraShaker::createSinglyHorizontalTask runs i < 7 over the 3-entry
mHorizontalTasks (this+0x28), a Nintendo bug that CodeWarrior compiled
literally: entries 3..6 land in mInfinityTasks (this+0x34), which
createInfinityTask then overwrites. GCC used the array bound to drop the exit
test, so the loop allocated CameraShake tasks until the FileSelect scene heap
was exhausted. The Wii U build bounds the loop to the array (TARGET_WIIU) with
no global -fno-aggressive-loop-optimizations to mask it: the linked function
must test its counter (a conditional backward branch) and store only into
this+0x28..0x33. sms_msl_sqrtf must not branch to itself (port_compat.h's sqrtf
macro inside msl_math.c made it "b .").
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
    if "-fno-aggressive-loop-optimizations" in args.cmake.read_text(encoding="utf-8"):
        print("ub-loops: FAIL: a global -fno-aggressive-loop-optimizations masks out-of-bounds loops")
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
        # Bounded behaviour: the task pointers are stored through a cursor
        # (stwu rC,step(rC)) set from this (r3) and compared against an end
        # pointer also set from this. Every store must land inside
        # mHorizontalTasks, this+0x28..0x33.
        base = {m.group(1): int(m.group(2)) for m in
                re.finditer(r"\baddi\s+(r\d+),r3,(-?\d+)", body)}
        store = next((m for m in re.finditer(r"\bstwu\s+(r\d+),(-?\d+)\((r\d+)\)", body)
                      if m.group(3) in base), None)
        cmp = re.search(r"\bcmplw\s+(?:cr\d,)?(r\d+),(r\d+)", body)
        if not store or not cmp or store.group(3) not in base:
            print("ub-loops: FAIL: createSinglyHorizontalTask store loop not recognised")
            return 1
        cursor = store.group(3)
        end_reg = cmp.group(2) if cmp.group(1) == cursor else cmp.group(1)
        if end_reg not in base:
            print("ub-loops: FAIL: createSinglyHorizontalTask loop end not recognised")
            return 1
        step = int(store.group(2))
        first, last = base[cursor] + step, base[end_reg]
        if first < 0x28 or last > 0x34 - 4:
            print(f"ub-loops: FAIL: createSinglyHorizontalTask stores this+{first:#x}..{last:#x}, "
                  "outside mHorizontalTasks (0x28..0x33)")
            return 1
        # sms_msl_sqrtf (platform/misc/msl_math.c) once became "b ." in the SMG
        # build: port_compat.h's sqrtf macro turned its libm call into itself.
        start = text.find("<sms_msl_sqrtf>:\n")
        body = text[start:text.find("\n\n", start)] if start >= 0 else ""
        for m in re.finditer(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*b\s+([0-9a-f]+)\b", body, re.M):
            if m.group(1) == m.group(2):
                print("ub-loops: FAIL: sms_msl_sqrtf branches to itself (sqrtf macro recursion)")
                return 1
        if "<sqrtf>" not in body:
            print("ub-loops: FAIL: sms_msl_sqrtf does not reach the C library's sqrtf")
            return 1
    print("ub-loops: PASS: retail out-of-bounds loops are bounded (no global loop-opt flag)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
