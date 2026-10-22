#!/usr/bin/env python3
"""Fail if the linked SMG Wii U ELF still dereferences Wii absolute addresses.

On the Wii the OS globals (boot info, bus clock, MEM2 end, module list...)
live in low MEM1 at 0x80000000 + offset, and hardware registers at
0xCC000000/0xCD000000. None of that is mapped for a Wii U RPX: a load or store
there faults (Cemu: SIGSEGV in PPCInterpreter_LWZ at JKRHeap::initArena).

This scans `objdump -d` of the *linked* smg.elf (run in the same builder
image) for the compiler's absolute-address pattern: `lis rN,0x8000|0xC000|
0xCC00|0xCD00` followed, before rN is redefined or a branch, by a load/store
whose base is rN (optionally via `addi`/`ori rM,rN,imm`). Every hit is reported as
function+offset and effective address. `--allow FUNC` exempts a function.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
READELF = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-readelf"
SECTION_RE = re.compile(r"\]\s+(\S+)\s+\S+(?:\+0x[0-9a-f]+)?\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)\s")
# wut's RPL import/export stubs are linked at 0xC0000000+; elf2rpl relocates
# them, so a lis 0xC000 into these sections is not a Wii hardware access.
RPL_SECTIONS = (".fimport_", ".dimport_", ".fexports", ".dexports", ".wut_load_bounds")

FUNC_RE = re.compile(r"^([0-9a-f]+) <(.+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")
MEM_RE = re.compile(r"^(r\d+|f\d+),(-?\d+)\((r\d+)\)$")
LOADS_STORES = {
    "lwz", "lwzu", "lhz", "lhzu", "lha", "lhau", "lbz", "lbzu",
    "stw", "stwu", "sth", "sthu", "stb", "stbu",
    "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu",
    "lmw", "stmw",
}
# lis immediates objdump prints as signed 16-bit values.
WII_HIGH = {-32768: 0x8000, -16384: 0xC000, -13312: 0xCC00, -13056: 0xCD00}


RAM_TEST_FUNCS = (
    "_ZN4nw4r3lyt6TexMap12ReplaceImageEP10TPLPalettej",
    "_ZN15DrawSyncManager10threadFuncEPv",
)


def function_body(text: str, name: str):
    start = text.find(f"<{name}>:\n")
    if start < 0:
        return None
    end = text.find("\n\n", start)
    return text[start:end if end > 0 else None]


def tool(elf: Path, use_docker: bool, binary: str, flag: str) -> str:
    if use_docker:
        cmd = [
            "docker", "run", "--rm", "--network", "none",
            "-v", f"{elf.parent}:/e:ro", BUILDER_IMAGE,
            binary, flag, f"/e/{elf.name}",
        ]
    else:
        cmd = [binary, flag, str(elf)]
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def scan(text: str):
    hits = []
    func, func_addr = "?", 0
    known: dict[str, int] = {}
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            func, func_addr = m.group(2), int(m.group(1), 16)
            known.clear()
            continue
        m = INSN_RE.match(line)
        if not m:
            continue
        addr, op, args = int(m.group(1), 16), m.group(2), m.group(3).split("<")[0].strip()
        if op.startswith("b"):
            known.clear()
            continue
        parts = [p.strip() for p in args.split(",")]
        if op == "lis" and len(parts) == 2:
            try:
                imm = int(parts[1], 0)
            except ValueError:
                imm = None
            if imm in WII_HIGH:
                known[parts[0]] = WII_HIGH[imm] << 16
            else:
                known.pop(parts[0], None)
            continue
        if op == "ori" and len(parts) == 3 and parts[1] in known:
            try:
                known[parts[0]] = known[parts[1]] | int(parts[2], 0)
            except ValueError:
                known.pop(parts[0], None)
            continue
        if op == "addi" and len(parts) == 3 and parts[1] in known:
            try:
                known[parts[0]] = (known[parts[1]] + int(parts[2], 0)) & 0xFFFFFFFF
            except ValueError:
                known.pop(parts[0], None)
            continue
        if op in LOADS_STORES:
            mm = MEM_RE.match(args.replace(" ", ""))
            if mm and mm.group(3) in known:
                ea = (known[mm.group(3)] + int(mm.group(2))) & 0xFFFFFFFF
                hits.append((func, addr - func_addr, op, ea))
            if mm and op.startswith("l") and mm.group(1) in known:
                known.pop(mm.group(1), None)
            continue
        if parts and parts[0] in known and not op.startswith(("cmp", "st", "tw", "dc", "ic")):
            known.pop(parts[0], None)
    return hits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--no-docker", action="store_true")
    parser.add_argument("--allow", action="append", default=[])
    parser.add_argument("--require-wiiu-ram-tests", action="store_true",
                        help="bound-pointer tests must use the Wii U RAM base (0x10000000)")
    args = parser.parse_args()
    elf = args.elf.resolve()
    docker = not args.no_docker
    rpl = []
    for m in SECTION_RE.finditer(tool(elf, docker, READELF, "-SW")):
        if m.group(1).startswith(RPL_SECTIONS):
            start = int(m.group(2), 16)
            rpl.append((start, start + int(m.group(3), 16)))
    if not rpl:
        print("lowmem: FAIL: no RPL import sections found (readelf parse)")
        return 1
    disassembly = tool(elf, docker, OBJDUMP, "-d")
    if args.require_wiiu_ram_tests:
        # port_is_ram_address(): `value >= 0x10000000` compiles to a compare
        # with lis 0x0fff (4095); the retail `>= 0x80000000` is a sign test.
        for func in RAM_TEST_FUNCS:
            body = function_body(disassembly, func)
            if body is None:
                print(f"lowmem: FAIL: {func} not found in the linked ELF")
                return 1
            if not re.search(r"\blis\s+r\d+,4095\b", body):
                print(f"lowmem: FAIL: {func} still tests pointers against the Wii base 0x80000000")
                return 1
    hits = [h for h in scan(disassembly)
            if not any(lo <= h[3] < hi for lo, hi in rpl)]
    bad = [h for h in hits if not any(a in h[0] for a in args.allow)]
    for func, off, op, ea in bad:
        print(f"lowmem: {func}+0x{off:x}: {op} 0x{ea:08x}")
    if bad:
        print(f"lowmem: FAIL: {len(bad)} absolute Wii address access(es) in the linked ELF")
        return 1
    print(f"lowmem: PASS: no absolute Wii low-memory/hardware access ({len(hits)} allowed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
