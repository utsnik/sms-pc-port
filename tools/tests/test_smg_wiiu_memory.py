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

    if args.mem1 is not None and mem1 != args.mem1:
        return fail(f"source MEM1 size={mem1}, expected {args.mem1}")
    if args.mem2 is not None and mem2 != args.mem2:
        return fail(f"source MEM2 size={mem2}, expected {args.mem2}")
    if args.alignment is not None and args.alignment != 64:
        return fail(f"test contract requires 64-byte target alignment, got {args.alignment}")

    print("memory: PASS: MEM1=24 MiB MEM2=64 MiB static aligned arenas")
    print(host_output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
