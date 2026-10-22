#!/usr/bin/env python3
"""Fail if Galaxy game code calls a coreinit thread API with its own OSThread.

Galaxy's OSThread is the game's (Wii SDK) struct; coreinit expects a Wii U
OSThread. Every thread call from game code must go through the port adapters
(platform/wiiu/os_thread.cpp, SMG_WIIU_OS_ADAPTER_DEFS). An unadapted
OSSetThreadPriority left the async executor at priority 1, so the priority-2
JASAudioThread never initialised JASDsp::FX_BUF before AudSystem used it.

The check scans the linked smg.elf: wut import calls are `lis r12,0xC000;
addi r12,r12,off; mtctr r12; bctr` trampolines into .fimport_coreinit. Any
coreinit import whose name contains "Thread" that is reached from a C++ game
function (mangled `_Z...`, not an anonymous-namespace port helper) fails.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
BIN = "/opt/devkitpro/devkitPPC/bin/"

FUNC_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
ADDI_RE = re.compile(r"addi\s+r12,r12,(-?\d+)")
# Port-side C++ helpers that legitimately hold wut threads.
PORT_PREFIXES = ("_ZN12_GLOBAL__N_1",)


def run(elf: Path, tool: str, flag: str) -> str:
    return subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
         BUILDER_IMAGE, BIN + tool, flag, f"/e/{elf.name}"],
        check=True, capture_output=True, text=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    args = parser.parse_args()
    elf = args.elf.resolve()
    imports = {}
    for line in run(elf, "powerpc-eabi-nm", "-n").splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0].startswith("c0000"):
            imports[int(parts[0], 16)] = parts[2]
    if not imports:
        print("thread-adapters: FAIL: no coreinit imports found")
        return 1
    lines = run(elf, "powerpc-eabi-objdump", "-d").splitlines()
    func = "?"
    bad = set()
    for i, line in enumerate(lines):
        m = FUNC_RE.match(line)
        if m:
            func = m.group(1)
            continue
        if "lis     r12,-16384" not in line or i + 1 >= len(lines):
            continue
        m = ADDI_RE.search(lines[i + 1])
        if not m:
            continue
        name = imports.get((0xC0000000 + int(m.group(1))) & 0xFFFFFFFF, "")
        if "Thread" not in name:
            continue
        if func.startswith("_Z") and not func.startswith(PORT_PREFIXES):
            bad.add((func, name))
    for func, name in sorted(bad):
        print(f"thread-adapters: {func} calls coreinit {name} directly")
    if bad:
        print(f"thread-adapters: FAIL: {len(bad)} unadapted game thread call(s)")
        return 1
    print("thread-adapters: PASS: game thread calls all go through the port adapters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
