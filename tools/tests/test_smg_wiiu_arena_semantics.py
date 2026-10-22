#!/usr/bin/env python3
"""Check the Galaxy Wii MEM1/MEM2 arena contract at its provider boundary."""

from pathlib import Path
import difflib
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
OS_CORE = ROOT / "platform/wiiu/os_core.cpp"
RUNTIME = ROOT / "games/smg/platform/wiiu/runtime.cpp"
ARALT = ROOT / "games/smg/decomp/src/RVL_SDK/aralt/aralt.c"
# 7e98c93 imported the retail Wii aralt.c (ruling R18: keep its semantics).
RETAIL_ARALT_COMMIT = "7e98c93"
ARALT_PATH = "games/smg/decomp/src/RVL_SDK/aralt/aralt.c"
# The only lines the port may add: the weak ARInit hook and its log.
ARALT_ALLOWED_ADDITIONS = {
    "extern void smg_wiiu_bind_varam_base(u32, u32) __attribute__((weak));",
    "extern void port_log(const char*, ...) __attribute__((weak));",
    "if (smg_wiiu_bind_varam_base)",
    "smg_wiiu_bind_varam_base(__ARH_BaseAdr, __AR_Size);",
    "if (port_log)",
    'port_log("[smg-audio] ARAM base=0x%08x size=0x%08x", __ARH_BaseAdr, __AR_Size);',
}


def fail(message: str) -> int:
    print(f"arena-semantics: FAIL: {message}")
    return 1


def body(source: str, signature: str) -> str:
    match = re.search(re.escape(signature) + r"\s*\{(?P<body>.*?)\n\}", source, re.S)
    return match.group("body") if match else ""


def aralt_retail_problem(aralt: str) -> str:
    """Return why aralt.c departs from the retail import, or ''."""
    result = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"{RETAIL_ARALT_COMMIT}:{ARALT_PATH}"],
        capture_output=True, text=True)
    if result.returncode:
        return f"cannot read retail aralt.c at {RETAIL_ARALT_COMMIT}: {result.stderr.strip()}"
    retail = result.stdout.splitlines()
    for line in difflib.ndiff(retail, aralt.splitlines()):
        tag, text = line[:2], line[2:].strip()
        if tag == "- ":
            return f"retail aralt.c line removed or changed: {text!r}"
        if tag == "+ ":
            if not text or text.startswith(("/*", "*", "//")):
                continue
            if text not in ARALT_ALLOWED_ADDITIONS:
                return f"non-retail aralt.c line added: {text!r}"
    arinit = body(aralt, "u32 ARInit(u32*, u32)")
    size_at = arinit.find("__AR_Size = diff;")
    bind_at = arinit.find("smg_wiiu_bind_varam_base(__ARH_BaseAdr, __AR_Size)")
    if size_at < 0 or bind_at < size_at:
        return "ARInit does not bind the retail base/size after __AR_Size = diff"
    return ""


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
    problem = aralt_retail_problem(aralt)
    if problem:
        return fail(problem)
    if "[smg-audio] ARAM base=" not in aralt:
        return fail("ARInit does not log the recorded base and size")
    print("arena-semantics: PASS: Wii arena cursors separated; aralt.c is retail + the ARInit bind/log hook")
    return 0


if __name__ == "__main__":
    sys.exit(main())
