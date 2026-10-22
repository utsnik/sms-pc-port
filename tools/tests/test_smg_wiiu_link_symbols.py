#!/usr/bin/env python3
"""Check that SMG's Galaxy input owns every symbol it defines in the ELF."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"input-link: FAIL: {message}")
    return 1


def defined_symbols(nm: Path, path: Path) -> list[str]:
    result = subprocess.run(
        [str(nm), "--defined-only", "--extern-only", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            symbols.append(fields[-1])
    return symbols


def audio_owners(map_path: Path, symbols: list[str]) -> dict[str, str]:
    lines = map_path.read_text(encoding="utf-8", errors="replace").splitlines()
    owners: dict[str, str] = {}
    wanted = set(symbols)
    for index, line in enumerate(lines):
        match = re.search(r"\.text\.(smg_gc_(?:AI|DSP)[A-Za-z0-9_]*)\s*$", line)
        if not match or match.group(1) not in wanted:
            continue
        symbol = match.group(1)
        for candidate in lines[index + 1:index + 6]:
            if "0x00000000" in candidate:
                continue
            object_match = re.search(r"(platform/[^ ]+\.o(?:\s|$))", candidate)
            if object_match:
                owners[symbol] = object_match.group(1).strip()
                break
    return owners


def check_audio_ownership(nm: Path, elf: Path, map_path: Path) -> int:
    symbols = [
        symbol for symbol in defined_symbols(nm, elf)
        if re.fullmatch(r"smg_gc_(?:AI|DSP)[A-Za-z0-9_]*", symbol)
    ]
    if not symbols:
        return fail("linked ELF has no reached smg_gc_AI*/DSP* symbols")
    duplicates = sorted(symbol for symbol in set(symbols) if symbols.count(symbol) != 1)
    if duplicates:
        return fail("audio symbols are not unique in the linked ELF: " + ", ".join(duplicates))
    owners = audio_owners(map_path, symbols)
    for symbol in sorted(set(symbols)):
        expected = "audio.o" if "_AI" in symbol else "dsp_hle.o"
        owner = owners.get(symbol, "")
        if not owner.endswith(expected):
            return fail(f"{symbol} is attributed to {owner or 'no map object'}, expected {expected}")
    print(f"audio-link: PASS: {len(symbols)} Galaxy AI/DSP symbols owned by audio.o/dsp_hle.o")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check Galaxy input ownership and SMG AI/DSP audio ownership.",
        epilog=(
            "Task 10/11 linked invocation: python3 tools/tests/test_smg_wiiu_link_symbols.py "
            "--elf $BUILD/smg.elf --map $BUILD/smg.map "
            "--provider-object $BUILD/cmake/platform/wiiu/CMakeFiles/smg.dir/__/__/games/smg/platform/wiiu/input.o "
            "--audio-elf $BUILD/smg.elf --audio-map $BUILD/smg.map "
            "--nm /opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm"
        ),
    )
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--provider-object", type=Path, required=True)
    parser.add_argument("--nm", type=Path, default=Path("powerpc-eabi-nm"))
    parser.add_argument("--audio-elf", type=Path)
    parser.add_argument("--audio-map", type=Path)
    args = parser.parse_args()

    for path in (args.elf, args.map, args.provider_object):
        if not path.is_file():
            return fail(f"missing audit input: {path}")
    try:
        expected = sorted(set(defined_symbols(args.nm, args.provider_object)))
        actual = defined_symbols(args.nm, args.elf)
    except (OSError, subprocess.CalledProcessError) as exc:
        return fail(f"nm failed: {exc}")

    if bool(args.audio_elf) != bool(args.audio_map):
        return fail("--audio-elf and --audio-map must be supplied together")
    if args.audio_elf and args.audio_map:
        for path in (args.audio_elf, args.audio_map):
            if not path.is_file():
                return fail(f"missing audio audit input: {path}")
        result = check_audio_ownership(args.nm, args.audio_elf, args.audio_map)
        if result:
            return result

    actual_counts = {symbol: actual.count(symbol) for symbol in expected}
    duplicate = [symbol for symbol, count in actual_counts.items() if count != 1]
    if duplicate:
        return fail("ELF does not have exactly one definition for: " + ", ".join(duplicate))

    map_lines = args.map.read_text(encoding="utf-8", errors="replace").splitlines()
    owned_symbols: set[str] = set()
    current_section_object = ""
    symbol_pattern = re.compile(r"^\s+0x[0-9a-f]+\s+(\S+)$")
    for line in map_lines:
        if re.match(r"^\s+\.[A-Za-z_]", line):
            current_section_object = line if "input.o" in line else ""
            continue
        match = symbol_pattern.match(line)
        if match and "input.o" in current_section_object:
            owned_symbols.add(match.group(1))
    provider_owned = []
    for symbol in expected:
        if symbol not in owned_symbols:
            return fail(f"link map does not attribute {symbol} to input.cpp")
        provider_owned.append(symbol)

    print(f"input-link: PASS: {len(provider_owned)} input.cpp symbols uniquely owned in ELF")
    return 0


if __name__ == "__main__":
    sys.exit(main())
