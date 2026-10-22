#!/usr/bin/env python3
"""Check SMG's static Wii-sized arenas and linked arena providers."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> int:
    print(f"memory: FAIL: {message}")
    return 1


def read_source(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"cannot read {path}: {exc}") from exc


def arena_bytes(source: str, name: str) -> int | None:
    match = re.search(
        rf"constexpr\s+size_t\s+{name}\s*=\s*(\d+)u?\s*\*\s*1024u?\s*\*\s*1024u?\s*;",
        source,
    )
    return int(match.group(1)) * 1024 * 1024 if match else None


def nm_info(elf: Path) -> tuple[dict[str, int], set[str]]:
    if not elf.is_file():
        raise RuntimeError(f"linked ELF is missing: {elf}")
    container_dir = "/elf"
    container_elf = f"{container_dir}/{elf.name}"
    result = subprocess.run(
        [
            "docker",
            "run",
            "--rm",
            "--network",
            "none",
            "--user",
            f"{os.getuid()}:{os.getgid()}",
            "-v",
            f"{elf.parent}:{container_dir}:ro",
            "-e",
            f"ELF={container_elf}",
            "soh-wiiu-builder:latest",
            "bash",
            "-lc",
            'set -euo pipefail; /opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm -C -S --defined-only "$ELF"',
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(
            "builder nm failed:\n" + (result.stdout + result.stderr).rstrip()
        )

    sizes: dict[str, int] = {}
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2] in {"T", "t"}:
            symbols.add(parts[-1])
        match = re.search(
            r"^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S\s+.*\b(s_mem[12]_arena)\s*$",
            line,
        )
        if match:
            sizes[match.group(2)] = int(match.group(1), 16)
    return sizes, symbols


# os_core.cpp's linked Wii arena delegates (ruling R12): which ArenaState
# field each getter returns / setter stores. Target pointers are 4 bytes, so
# field k of smg_wiiu::ArenaState is at 4*k (mem1_start, mem1_end, mem1_lo,
# mem1_hi, mem2_start, mem2_end, mem2_lo, mem2_hi).
MEM1_FIELDS = range(0, 16)
MEM2_FIELDS = range(16, 32)
ARENA_GETTERS = {
    "OSGetArenaLo": 8, "OSGetArenaHi": 12,
    "OSGetMEM1ArenaLo": 8, "OSGetMEM1ArenaHi": 12,
    "OSGetMEM2ArenaLo": 24, "OSGetMEM2ArenaHi": 28,
}
ARENA_SETTERS = {
    "OSSetArenaLo": (8, MEM1_FIELDS), "OSSetArenaHi": (12, MEM1_FIELDS),
    "OSSetMEM2ArenaLo": (24, MEM2_FIELDS), "OSSetMEM2ArenaHi": (28, MEM2_FIELDS),
}
ARENA_STATE_SYMBOL = "_ZN12_GLOBAL__N_1L12s_smg_arenasE"


def disassemble_arena_api(elf: Path) -> tuple[int, dict[str, list[str]]]:
    names = [ARENA_STATE_SYMBOL, *ARENA_GETTERS, *ARENA_SETTERS]
    script = (
        'set -euo pipefail; BIN=/opt/devkitpro/devkitPPC/bin; '
        '$BIN/powerpc-eabi-nm -S --defined-only "$ELF" | '
        'awk \'{print $1, $2, $NF}\' > /tmp/syms; '
        'for name in ' + " ".join(names) + '; do '
        'line=$(awk -v n="$name" \'$3==n{print; exit}\' /tmp/syms); '
        'test -n "$line" || { echo "MISSING $name"; continue; }; '
        'set -- $line; echo "SYM $3 $1 $2"; '
        'if [ "$3" != ' + ARENA_STATE_SYMBOL + ' ]; then '
        '$BIN/powerpc-eabi-objdump -d --no-show-raw-insn --start-address=0x$1 '
        '--stop-address=$(printf 0x%x $((0x$1 + 0x$2))) "$ELF" | '
        'sed -n "s/^ *[0-9a-f]*:\t/INS $3 /p"; fi; done'
    )
    result = subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "--user",
         f"{os.getuid()}:{os.getgid()}", "-v", f"{elf.parent}:/elf:ro",
         "-e", f"ELF=/elf/{elf.name}", "soh-wiiu-builder:latest", "bash", "-lc", script],
        capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("builder objdump failed:\n" + (result.stdout + result.stderr).rstrip())
    base = None
    code: dict[str, list[str]] = {}
    for line in result.stdout.splitlines():
        if line.startswith("MISSING "):
            raise RuntimeError("linked ELF has no " + line.split()[1])
        parts = line.split(None, 2)
        if parts[0] == "SYM" and parts[1] == ARENA_STATE_SYMBOL:
            base = int(line.split()[2], 16)
        elif parts[0] == "INS":
            code.setdefault(parts[1], []).append(parts[2])
    if base is None:
        raise RuntimeError(f"linked ELF has no {ARENA_STATE_SYMBOL}")
    return base, code


def arena_accesses(instructions: list[str]) -> list[tuple[str, str, int]]:
    """Resolve lis/addi-based lwz/stw addresses: (op, data register, address)."""
    known: dict[str, int] = {}
    accesses = []
    for text in instructions:
        fields = text.replace(",", " ").split()
        op, operands = fields[0], fields[1:]
        memory = re.match(r"^(-?\d+)\((r\d+)\)$", operands[-1]) if operands else None
        if op == "lis":
            known[operands[0]] = (int(operands[1]) << 16) & 0xFFFFFFFF
            continue
        if op == "addi" and operands[1] in known:
            known[operands[0]] = (known[operands[1]] + int(operands[2])) & 0xFFFFFFFF
            continue
        if op in ("lwz", "stw") and memory and memory.group(2) in known:
            address = (known[memory.group(2)] + int(memory.group(1))) & 0xFFFFFFFF
            accesses.append((op, operands[0], address))
        if op in ("bl", "bla", "blrl", "bctrl"):
            for reg in ["r0"] + [f"r{i}" for i in range(3, 13)]:
                known.pop(reg, None)
        elif operands and not op.startswith(("st", "cmp", "b")):
            known.pop(operands[0], None)
    return accesses


def arena_delegate_check(elf: Path) -> str:
    base, code = disassemble_arena_api(elf)
    for name, field in ARENA_GETTERS.items():
        loads = [a for op, reg, a in arena_accesses(code.get(name, [])) if op == "lwz" and reg == "r3"]
        if loads != [base + field]:
            got = ", ".join(f"+{a - base}" for a in loads) or "none"
            raise RuntimeError(f"linked {name} returns ArenaState {got}, expected +{field}")
    for name, (field, allowed) in ARENA_SETTERS.items():
        accesses = arena_accesses(code.get(name, []))
        stores = [a for op, _reg, a in accesses if op == "stw"]
        if stores != [base + field]:
            got = ", ".join(f"+{a - base}" for a in stores) or "none"
            raise RuntimeError(f"linked {name} stores ArenaState {got}, expected +{field}")
        stray = [a - base for _op, _reg, a in accesses if (a - base) not in allowed]
        if stray:
            raise RuntimeError(f"linked {name} touches the other arena's cursor fields {stray}")
    return (f"memory-elf: PASS: linked os_core delegates read/write ArenaState "
            f"MEM1 (OSGet/SetArena*, OSGetMEM1Arena*) and MEM2 (OS*MEM2Arena*) fields")


def host_allocator_check(os_core: Path) -> str:
    harness = Path(__file__).with_name("smg_wiiu_memory_host.cpp")
    with tempfile.TemporaryDirectory(prefix="smg-memory-") as build_dir:
        executable = Path(build_dir) / "smg_wiiu_memory_host"
        compile_result = subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(os_core.parent),
                str(harness),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            raise RuntimeError(
                "host allocator harness compile failed:\n"
                + compile_result.stdout
                + compile_result.stderr
            )
        run_result = subprocess.run(
            [str(executable)], capture_output=True, text=True
        )
        output = run_result.stdout + run_result.stderr
        if run_result.returncode:
            raise RuntimeError(
                f"host allocator harness failed ({run_result.returncode}):\n"
                + output.rstrip()
            )
        return output.rstrip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--smg-runtime", type=Path, required=True)
    parser.add_argument("--os-core", type=Path, required=True)
    parser.add_argument("--mem1", type=int)
    parser.add_argument("--mem2", type=int)
    parser.add_argument("--alignment", type=int)
    parser.add_argument(
        "--elf", type=Path, default=Path("/mnt/buildcache/wiiu-build/smg/smg.elf")
    )
    args = parser.parse_args()

    try:
        runtime = read_source(args.smg_runtime)
        os_core = read_source(args.os_core)
    except RuntimeError as exc:
        return fail(str(exc))

    mem1 = arena_bytes(runtime, "kMem1ArenaBytes")
    mem2 = arena_bytes(runtime, "kMem2ArenaBytes")
    if mem1 is None or mem2 is None:
        return fail("missing explicit MEM1/MEM2 arena sizes")
    if "s_arena[kArenaBytes]" in runtime or "kArenaBytes = 16u * 1024u * 1024u" in runtime:
        return fail("Stage 1 single 16 MiB arena remains")
    if not re.search(r"alignas\(64\)\s+unsigned char\s+s_mem1_arena\[", runtime):
        return fail("MEM1 arena is not a static 64-byte-aligned array")
    if not re.search(r"alignas\(64\)\s+unsigned char\s+s_mem2_arena\[", runtime):
        return fail("MEM2 arena is not a static 64-byte-aligned array")
    if "MEMAllocFromDefaultHeapEx" in runtime:
        return fail("arena ownership still uses MEMAllocFromDefaultHeapEx")
    if "SMG Wii U: arenas MEM1=25165824 MEM2=67108864 alignment=64" not in runtime:
        return fail("missing exact arena target log line")
    if not re.search(r"assert\s*\(", runtime):
        return fail("missing host-checkable arena alignment assertion")
    try:
        host_output = host_allocator_check(args.os_core)
    except RuntimeError as exc:
        return fail(str(exc))

    required_providers = (
        "OSGetMEM1ArenaLo",
        "OSGetMEM1ArenaHi",
        "OSGetMEM2ArenaLo",
        "OSGetMEM2ArenaHi",
        "OSSetMEM2ArenaLo",
        "OSSetMEM2ArenaHi",
        "OSGetArenaLo",
        "OSGetArenaHi",
        "OSAllocFromMEM1ArenaLo",
        "OSAllocFromArenaLo",
        "OSAllocFromArenaHi",
    )
    missing = [name for name in required_providers if not re.search(rf"\b{name}\s*\(", os_core)]
    if missing:
        return fail("missing real arena providers: " + ", ".join(missing))
    try:
        stubs = read_source(args.smg_runtime.parent / "sdk_stubs.h")
    except RuntimeError as exc:
        return fail(str(exc))
    if re.search(r"X\((?:OSSetMEM2ArenaHi|OSGetMEM2Arena(?:Lo|Hi))\)", stubs):
        return fail("arena providers remain in the finite Stage 1 stub list")

    try:
        sizes, symbols = nm_info(args.elf)
    except RuntimeError as exc:
        return fail(str(exc))
    expected_sizes = {"s_mem1_arena": mem1, "s_mem2_arena": mem2}
    for name, expected in expected_sizes.items():
        actual = sizes.get(name)
        if actual != expected:
            return fail(f"linked {name} size={actual!r}, expected {expected}")
    missing_linked = [name for name in required_providers if name not in symbols]
    if missing_linked:
        return fail("linked ELF is missing real arena providers: " + ", ".join(missing_linked))

    try:
        delegate_output = arena_delegate_check(args.elf)
    except RuntimeError as exc:
        return fail(str(exc))

    if args.mem1 is not None and mem1 != args.mem1:
        return fail(f"source MEM1 size={mem1}, expected {args.mem1}")
    if args.mem2 is not None and mem2 != args.mem2:
        return fail(f"source MEM2 size={mem2}, expected {args.mem2}")
    if args.alignment is not None and args.alignment != 64:
        return fail(f"test contract requires 64-byte target alignment, got {args.alignment}")

    print("memory: PASS: MEM1=24 MiB MEM2=64 MiB static aligned arenas")
    print(host_output)
    print(delegate_output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
