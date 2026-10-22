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
    parser.add_argument("--require-queue-reclaim", action="store_true",
                        help="the OSThreadQueue side table must be the 512-record SMG table "
                             "(records are reclaimed when no thread sleeps on them)")
    parser.add_argument("--require-audio-init-wait", action="store_true",
                        help="JASAudioThread::create must wait (OSSleepTicks) for the audio thread's init")
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
    calls = set()
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
        calls.add((func, name))
        if "Thread" not in name and "Alarm" not in name:
            continue
        if func.startswith("_Z") and not func.startswith(PORT_PREFIXES):
            bad.add((func, name))
    if args.require_queue_reclaim:
        # 128 records were never released: Galaxy's per-request message queues
        # exhausted the table and every later sleep/wake was dropped.
        sizes = {}
        for line in run(elf, "powerpc-eabi-nm", "-S").splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[3].endswith("8s_queuesE"):
                sizes["s_queues"] = int(parts[1], 16)
        # QueueRecord: game*, WutQueue (16), active, sleepers = 28 bytes.
        if sizes.get("s_queues", 0) < 512 * 28:
            print("thread-adapters: FAIL: OSThreadQueue side table is not the reclaiming 512-record table")
            return 1
    if args.require_audio_init_wait:
        # Retail order: the priority-2 audio thread finishes DSP boot + FX_BUF
        # before its creator reaches JAUNewSectionHeap (which takes all of
        # JASDram). create() must sleep until run() reports the init done.
        if ("_ZN14JASAudioThread6createEi", "OSSleepTicks") not in calls:
            print("thread-adapters: FAIL: JASAudioThread::create does not wait for the audio thread init")
            return 1
    for func, name in sorted(bad):
        print(f"thread-adapters: {func} calls coreinit {name} directly")
    if bad:
        print(f"thread-adapters: FAIL: {len(bad)} unadapted game thread call(s)")
        return 1
    print("thread-adapters: PASS: game thread calls all go through the port adapters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
