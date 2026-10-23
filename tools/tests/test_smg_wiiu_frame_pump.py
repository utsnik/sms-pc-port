#!/usr/bin/env python3
"""Ruling R15: Galaxy's retrace wait must drive the platform frame boundary.

On the Wii, JUTVideo's message queue is filled from the retrace interrupt
(JUTVideo::postRetraceProc). On Wii U the VI callbacks fire only inside the
platform frame boundary (platform/wiiu/vi.cpp sms_wiiu_frame_boundary, reached
through VIFlush/VIWaitForRetrace on the main thread), so a blocking receive in
MainLoopFramework's waitForTick slept forever on a queue only that thread can
fill. Check the linked ELF: waitForTick must call VIWaitForRetrace.

--require-no-nested-boundary: JUTVideo::preRetraceProc calls VIFlush from the
pre-retrace callback, i.e. from inside the boundary; that nested one boundary
per vsync forever. sms_wiiu_frame_boundary, VIFlush and VIWaitForRetrace must
each read vi.cpp's s_in_boundary flag.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
WAIT_FOR_TICK = re.compile(r"^[0-9a-f]+ <(_ZN12_GLOBAL__N_1L?11waitForTickEjt[^>]*)>:$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--require-no-nested-boundary", action="store_true")
    parser.add_argument("--require-paced-retrace", action="store_true",
                        help="sms_wiiu_vsync_fields must pace (OSSleepTicks) and never freeze the retrace count")
    args = parser.parse_args()
    elf = args.elf.resolve()

    def tool(binary: str, flag: str) -> str:
        return subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
             BUILDER_IMAGE, binary, flag, f"/e/{elf.name}"],
            check=True, capture_output=True, text=True).stdout

    text = tool(OBJDUMP, "-d")
    if args.require_paced_retrace:
        # Without GX2 the retrace count must still advance at 60 Hz: a vsync
        # source returning 0 froze VIGetRetraceCount and waitForTick spun.
        imports = {}
        for line in tool(OBJDUMP.replace("objdump", "nm"), "-n").splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0].startswith("c0000"):
                imports[parts[2]] = int(parts[0], 16)
        start = text.find("<sms_wiiu_vsync_fields>:\n")
        body = text[start:text.find("\n\n", start)] if start >= 0 else ""
        sleep = imports.get("OSSleepTicks")
        off = None if sleep is None else sleep - 0xC0000000
        if not body or re.fullmatch(r"(?s).*>:\n\s+[0-9a-f]+:\s+38 60 00 00\s+li\s+r3,0\n\s+[0-9a-f]+:\s+4e 80 00 20\s+blr\s*", body) \
                or off is None or not re.search(rf"addi\s+r12,r12,{off}\b", body):
            print("frame-pump: FAIL: sms_wiiu_vsync_fields does not pace the retrace count (OSSleepTicks)")
            return 1
    if args.require_no_nested_boundary:
        flag_addr = None
        for line in tool(OBJDUMP.replace("objdump", "nm"), "-C").splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-1].endswith("s_in_boundary"):
                flag_addr = int(parts[0], 16)
        if flag_addr is None:
            print("frame-pump: FAIL: vi.cpp has no s_in_boundary re-entry flag")
            return 1
        hi = (flag_addr + 0x8000) >> 16
        lo = flag_addr - (hi << 16)
        hi_s = hi - 0x10000 if hi >= 0x8000 else hi
        for name in ("sms_wiiu_frame_boundary", "VIFlush", "VIWaitForRetrace"):
            start = text.find(f"<{name}>:\n")
            body = text[start:text.find("\n\n", start)] if start >= 0 else ""
            if not (re.search(rf"\blis\s+r\d+,{hi_s}\b", body) and
                    re.search(rf"\b(lbz|stb)\s+r\d+,{lo}\(r\d+\)", body)):
                print(f"frame-pump: FAIL: {name} does not check s_in_boundary (nested boundary)")
                return 1
    func = None
    bodies: dict[str, list[str]] = {}
    for line in text.splitlines():
        if line.endswith(">:"):
            m = WAIT_FOR_TICK.match(line)
            func = m.group(1) if m else None
            if func:
                bodies[func] = []
            continue
        if func:
            bodies[func].append(line)
    if not bodies:
        print("frame-pump: FAIL: waitForTick not found in the linked ELF")
        return 1
    if not any("<VIWaitForRetrace>" in l for body in bodies.values() for l in body):
        print("frame-pump: FAIL: waitForTick blocks on JUTVideo's queue without driving VIWaitForRetrace")
        return 1
    print("frame-pump: PASS: waitForTick drives the platform frame boundary (VIWaitForRetrace)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
