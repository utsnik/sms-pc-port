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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--provider-object", type=Path, required=True)
    parser.add_argument("--nm", type=Path, default=Path("powerpc-eabi-nm"))
    args = parser.parse_args()

    for path in (args.elf, args.map, args.provider_object):
        if not path.is_file():
            return fail(f"missing audit input: {path}")
    try:
        expected = sorted(set(defined_symbols(args.nm, args.provider_object)))
        actual = defined_symbols(args.nm, args.elf)
    except (OSError, subprocess.CalledProcessError) as exc:
        return fail(f"nm failed: {exc}")

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
