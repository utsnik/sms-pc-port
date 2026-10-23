#!/usr/bin/env python3
"""Galaxy's wchar_t must be 16 bits on Wii U, with 16-bit wide-string functions.

CodeWarrior's wchar_t is 16 bits and Galaxy reads its UTF-16 message data
through wchar_t*. devkitPPC's wchar_t is 32 bits unless -fshort-wchar is given:
MR::getStringLengthWithMessageTag then walked the 16-bit message in 32-bit
units and never found its terminator (the main thread spun in
LayoutCoreUtil::initTextBoxPane for "Layout_DPDPointerNumberP1").

Host part: compile games/smg/platform/wiiu/wstring.cpp with -fshort-wchar and
check the functions. --elf part: getStringLengthWithMessageTag reads 16-bit
units (lhz), and no Galaxy game function calls newlib's 32-bit wide functions.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
NAMES = ("wcslen", "wcsncpy", "wcscpy", "wcscmp", "wcsncmp", "wcschr", "wcscat",
         "swprintf", "vswprintf")

HOST_MAIN = r"""
#include <stdio.h>
#include <string.h>
extern "C" size_t port_wcslen(const wchar_t*);
extern "C" int port_wcscmp(const wchar_t*, const wchar_t*);
extern "C" int port_swprintf(wchar_t*, size_t, const wchar_t*, ...);
int main() {
    static_assert(sizeof(wchar_t) == 2, "-fshort-wchar");
    const wchar_t msg[] = L"Mario";
    int fails = 0;
    if (port_wcslen(msg) != 5) { puts("FAIL wcslen"); fails++; }
    wchar_t buf[32];
    port_swprintf(buf, 32, L"%ls %d", msg, 7);
    if (port_wcscmp(buf, L"Mario 7")) { puts("FAIL swprintf"); fails++; }
    puts(fails ? "host: FAIL" : "host: PASS");
    return fails;
}
"""


def fail(message: str) -> int:
    print(f"wide-strings: FAIL: {message}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shim", type=Path, required=True)
    parser.add_argument("--compat", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    if "-fshort-wchar" not in args.cmake.read_text(encoding="utf-8"):
        return fail("SMG game flags do not set -fshort-wchar")
    compat = args.compat.read_text(encoding="utf-8")
    for name in NAMES:
        if f'__asm__("port_{name}")' not in compat:
            return fail(f"compat.h does not bind {name} to port_{name}")
    with tempfile.TemporaryDirectory() as tmp:
        main_cpp = Path(tmp) / "main.cpp"
        main_cpp.write_text(HOST_MAIN, encoding="utf-8")
        exe = Path(tmp) / "t"
        build = subprocess.run(["g++", "-fshort-wchar", "-Wno-format", str(args.shim), str(main_cpp),
                                "-o", str(exe)], capture_output=True, text=True)
        if build.returncode:
            return fail("host build: " + build.stderr[-400:])
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout.strip())
        if run.returncode:
            return fail("host wide-string behaviour")
    if args.elf:
        elf = args.elf.resolve()
        text = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
             BUILDER_IMAGE, OBJDUMP, "-d", f"/e/{elf.name}"],
            check=True, capture_output=True, text=True).stdout
        start = text.find("<_ZN2MR29getStringLengthWithMessageTagEPKw>:\n")
        if start < 0:
            return fail("MR::getStringLengthWithMessageTag not found")
        body = text[start:text.find("\n\n", start)]
        if not re.search(r"\blhz\b", body) or re.search(r"\blwz\s+r\d+,0\(r3\)", body):
            return fail("getStringLengthWithMessageTag does not read 16-bit units")
        func = "?"
        bad = set()
        call = re.compile(r"\bbl\s+[0-9a-f]+ <(" + "|".join(NAMES) + r")>")
        for line in text.splitlines():
            m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
            if m:
                func = m.group(1)
                continue
            if func.startswith("_Z") and call.search(line):
                bad.add(func)
        if bad:
            return fail("game code calls newlib wide functions: " + ", ".join(sorted(bad)[:6]))
        print("elf: PASS: 16-bit message walk; no game caller of newlib wide functions")
    print("wide-strings: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
