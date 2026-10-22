#!/usr/bin/env python3
"""Check Galaxy's MSL printf semantics on the Wii U target.

Galaxy relies on CodeWarrior MSL printf: a NULL "%s" prints nothing. newlib
prints "(null)", so LayoutHolder::count built "(null)/anim", got a null
JKRArcFinder and Cemu faulted in the interpreter. compat.h's rename macros are
#undef'd by libstdc++'s <cstdio>, so on Wii U the binding is an asm label.

Host part: compile games/smg/platform/wiiu/msl_printf.cpp with g++ and check
its formatting. --elf part: the linked smg.elf's LayoutHolder::count must call
port_sprintf, and no Galaxy game function may call newlib's sprintf family.
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

HOST_MAIN = r"""
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
extern "C" int port_sprintf(char*, const char*, ...);
extern "C" int port_snprintf(char*, size_t, const char*, ...);
static int fails;
static void expect(const char* got, const char* want) {
    if (strcmp(got, want)) { printf("FAIL: got '%s' want '%s'\n", got, want); fails++; }
}
int main() {
    char b[256];
    const char* none = nullptr;
    port_sprintf(b, "%s%s%s", none, "/", "anim"); expect(b, "/anim");
    port_sprintf(b, "[%5s|%-3s]", none, "x"); expect(b, "[     |x  ]");
    port_sprintf(b, "%d %u %x %ld %lu", -5, 7u, 255u, (long)-9, (unsigned long)9); expect(b, "-5 7 ff -9 9");
    port_sprintf(b, "%zu %lld %.2f %c %%", (size_t)42, (long long)1 << 40, 1.5, 'q');
    expect(b, "42 1099511627776 1.50 q %");
    const uint16_t wide[] = {'R', 'M', 'G', 0};
    port_sprintf(b, "%ls", wide); expect(b, "RMG");
    int n = port_snprintf(b, 4, "%s", "abcdef"); expect(b, "abc");
    if (n != 6) { printf("FAIL: snprintf returned %d\n", n); fails++; }
    puts(fails ? "host: FAIL" : "host: PASS");
    return fails ? 1 : 0;
}
"""

FUNC_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
CALL_RE = re.compile(r"\bbl\s+[0-9a-f]+ <(port_)?(v?s?n?printf)>")


def fail(message: str) -> int:
    print(f"msl-printf: FAIL: {message}")
    return 1


def host_check(shim: Path) -> str | None:
    with tempfile.TemporaryDirectory() as tmp:
        main = Path(tmp) / "main.cpp"
        main.write_text(HOST_MAIN, encoding="utf-8")
        exe = Path(tmp) / "t"
        build = subprocess.run(
            ["g++", "-std=c++17", "-DPORT_PLATFORM_CODE=1", "-Wno-format",
             str(shim), str(main), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode:
            return "host build failed: " + build.stderr[-400:]
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout.strip())
        if run.returncode:
            return "host formatting differs from MSL"
    return None


def elf_check(elf: Path, game_markers: list[str]) -> str | None:
    text = subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
         BUILDER_IMAGE, OBJDUMP, "-d", f"/e/{elf.name}"],
        check=True, capture_output=True, text=True).stdout
    func = "?"
    layout_calls = set()
    newlib_callers = set()
    for line in text.splitlines():
        m = FUNC_RE.match(line)
        if m:
            func = m.group(1)
            continue
        m = CALL_RE.search(line)
        if not m:
            continue
        if func == "_ZN12LayoutHolder5countEPKcS1_":
            layout_calls.add((m.group(1) or "") + m.group(2))
        # Galaxy game code is C++ in Galaxy namespaces/classes (mangled _Z...).
        if not m.group(1) and func.startswith("_Z") and any(k in func for k in game_markers):
            newlib_callers.add(func)
    if layout_calls != {"port_sprintf"}:
        return f"LayoutHolder::count calls {sorted(layout_calls)} (want port_sprintf)"
    if newlib_callers:
        return "game code calls newlib printf: " + ", ".join(sorted(newlib_callers)[:8])
    print("elf: PASS: LayoutHolder::count -> port_sprintf; no game caller of newlib printf")
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shim", type=Path, required=True)
    parser.add_argument("--compat", type=Path, required=True)
    parser.add_argument("--elf", type=Path)
    args = parser.parse_args()
    compat = args.compat.read_text(encoding="utf-8")
    for name in ("sprintf", "snprintf", "vsprintf", "vsnprintf"):
        if f'__asm__("port_{name}")' not in compat:
            return fail(f"compat.h does not bind {name} to port_{name}")
    error = host_check(args.shim)
    if error:
        return fail(error)
    if args.elf:
        error = elf_check(args.elf.resolve(), ["Holder", "Layout", "Scene", "Galaxy", "MR"])
        if error:
            return fail(error)
    print("msl-printf: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
