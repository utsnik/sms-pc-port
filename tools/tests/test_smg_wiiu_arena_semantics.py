#!/usr/bin/env python3
"""Check the Galaxy Wii MEM1/MEM2 arena contract at its provider boundary."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
OS_CORE = ROOT / "platform/wiiu/os_core.cpp"
RUNTIME = ROOT / "games/smg/platform/wiiu/runtime.cpp"
ARALT = ROOT / "games/smg/decomp/src/RVL_SDK/aralt/aralt.c"


def fail(message: str) -> int:
    print(f"arena-semantics: FAIL: {message}")
    return 1


def body(source: str, signature: str) -> str:
    match = re.search(re.escape(signature) + r"\s*\{(?P<body>.*?)\n\}", source, re.S)
    return match.group("body") if match else ""


def main() -> int:
    os_core = OS_CORE.read_text(encoding="utf-8")
    runtime = RUNTIME.read_text(encoding="utf-8")
    aralt = ARALT.read_text(encoding="utf-8")

    if "s_arena_lo = s_mem2_arena_start + smg_wiiu::kAramWindowBytes" in os_core:
        return fail("port_os_init still hides the MEM2 VARAM window from Galaxy")
    if not re.search(r"OSGetArenaLo\(void\).*?return s_mem1_arena_lo;", os_core, re.S):
        return fail("Wii OSGetArenaLo is not the MEM1 cursor")
    if not re.search(r"OSGetArenaHi\(void\).*?return s_mem1_arena_hi;", os_core, re.S):
        return fail("Wii OSGetArenaHi is not the MEM1 cursor")
    if not re.search(r"OSGetMEM2ArenaLo\(void\).*?return s_mem2_arena_lo;", os_core, re.S):
        return fail("MEM2 has no independent low cursor")
    if not re.search(r"OSGetMEM2ArenaHi\(void\).*?return s_mem2_arena_hi;", os_core, re.S):
        return fail("MEM2 has no independent high cursor")
    if "void OSSetMEM2ArenaLo(void*" not in os_core:
        return fail("MEM2 has no low-cursor setter")
    if "s_port_pool" not in runtime:
        return fail("port allocations do not have a private pool")
    if "s_mem2_arena_used" in runtime:
        return fail("port allocations still use the Galaxy-visible MEM2 cursor")
    if "smg_wiiu_bind_varam_base(__ARH_BaseAdr, __AR_Size)" not in aralt:
        return fail("ARInit does not bind the real base and AR size")
    if "[smg-audio] ARAM base=" not in aralt:
        return fail("ARInit does not log the recorded base and size")
    print("arena-semantics: PASS: Wii arena cursors and private port pool are separated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
