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
    if "smg_wiiu::init(s_smg_arenas" not in os_core:
        return fail("os_core does not initialize the shared arena provider")
    if not re.search(r"OSGetArenaLo\(void\).*?smg_wiiu::arena_lo\(s_smg_arenas\)", os_core, re.S):
        return fail("Wii OSGetArenaLo is not the shared MEM1 cursor")
    if not re.search(r"OSGetArenaHi\(void\).*?smg_wiiu::arena_hi\(s_smg_arenas\)", os_core, re.S):
        return fail("Wii OSGetArenaHi is not the shared MEM1 cursor")
    if not re.search(r"OSGetMEM2ArenaLo\(void\).*?smg_wiiu::mem2_lo\(s_smg_arenas\)", os_core, re.S):
        return fail("MEM2 has no independent shared low cursor")
    if not re.search(r"OSGetMEM2ArenaHi\(void\).*?smg_wiiu::mem2_hi\(s_smg_arenas\)", os_core, re.S):
        return fail("MEM2 has no independent shared high cursor")
    if "void OSSetMEM2ArenaLo(void*" not in os_core:
        return fail("MEM2 has no low-cursor setter")
    if "s_port_pool" in runtime or "port_pool_alloc" in runtime:
        return fail("port allocations still use the deleted bump pool")
    if "s_mem2_arena_used" in runtime:
        return fail("port allocations still use the Galaxy-visible MEM2 cursor")
    if "smg_wiiu_bind_varam_base(__ARH_BaseAdr, __AR_Size)" not in aralt:
        return fail("ARInit does not bind the real base and AR size")
    if "__AR_Size = diff + ARALT_RESERVED_BYTES" not in aralt:
        return fail("ARInit does not include the 0x4000 ARAM reservation in ARGetSize")
    if "[smg-audio] ARAM base=" not in aralt:
        return fail("ARInit does not log the recorded base and size")
    print("arena-semantics: PASS: Wii arena cursors and private port pool are separated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
