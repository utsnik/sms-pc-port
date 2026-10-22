#!/usr/bin/env python3
"""Check SMG linked-ELF ownership: Galaxy input symbols, AI/DSP audio
providers, and the port_heap.o operator new/delete --wrap (ruling R17)."""

from __future__ import annotations

import argparse
import bisect
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


# Ruling R17: the smg link wraps Galaxy's global operator new/delete exactly
# like Sunshine's sms link, plus Galaxy's own sized array delete (JKRHeap.cpp
# defines _ZdaPvj directly; libsupc++'s forwarding copy is never pulled in).
HEAP_WRAPPED = ("_Znwj", "_Znaj", "_ZdlPv", "_ZdaPv", "_ZdlPvj", "_ZdaPvj")


def kept_text_sections(map_path: Path) -> list[tuple[str, int, int, str]]:
    """Return (section, address, size, object) for every linked .text input."""
    lines = map_path.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        start = next(i for i, line in enumerate(lines)
                     if line.startswith("Linker script and memory map"))
    except StopIteration:
        raise RuntimeError(f"{map_path} has no 'Linker script and memory map' section")
    sections = []
    row = re.compile(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$")
    index = start
    while index < len(lines):
        line = lines[index]
        same = re.match(r"^ (\.text\S*)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$", line)
        if same:
            sections.append((same.group(1), int(same.group(2), 16),
                             int(same.group(3), 16), same.group(4).strip()))
        else:
            alone = re.match(r"^ (\.text\S*)\s*$", line)
            if alone and index + 1 < len(lines):
                follow = row.match(lines[index + 1])
                if follow:
                    sections.append((alone.group(1), int(follow.group(1), 16),
                                     int(follow.group(2), 16), follow.group(3).strip()))
                    index += 1
        index += 1
    return [section for section in sections if section[2] > 0]


def heap_fail(message: str) -> int:
    print(f"heap-wrap: FAIL: {message}")
    return 1


def check_heap_wrap(nm: Path, objdump: Path, elf: Path, map_path: Path) -> int:
    try:
        sections = kept_text_sections(map_path)
    except RuntimeError as exc:
        return heap_fail(str(exc))
    owners: dict[str, list[str]] = {}
    for name, _address, _size, owner in sections:
        owners.setdefault(name, []).append(owner)
    for symbol in HEAP_WRAPPED:
        wrap_owner = owners.get(f".text.__wrap_{symbol}", [])
        if len(wrap_owner) != 1 or not wrap_owner[0].endswith("port_heap.o"):
            return heap_fail(f"__wrap_{symbol} is not linked from port_heap.o "
                        f"(kept owners: {wrap_owner or 'none: --wrap inactive, section gc-ed'})")
        real_owner = owners.get(f".text.{symbol}", [])
        if len(real_owner) != 1 or not real_owner[0].endswith("(JKRHeap.o)"):
            return heap_fail(f"{symbol} (the __real_ target) is not Galaxy's JKRHeap.o "
                        f"definition (kept owners: {real_owner or 'none'})")

    defined = set(defined_symbols(nm, elf))
    missing = [f"__wrap_{symbol}" for symbol in HEAP_WRAPPED if f"__wrap_{symbol}" not in defined]
    if missing:
        return heap_fail("linked ELF does not define " + ", ".join(missing))

    starts = sorted(sections, key=lambda section: section[1])
    addresses = [section[1] for section in starts]

    def owner_of(address: int) -> str:
        index = bisect.bisect_right(addresses, address) - 1
        if index >= 0 and starts[index][1] <= address < starts[index][1] + starts[index][2]:
            return starts[index][3]
        return "?"

    result = subprocess.run([str(objdump), "-d", "--no-show-raw-insn", str(elf)],
                            capture_output=True, text=True)
    if result.returncode:
        return heap_fail("objdump failed: " + result.stderr.strip())
    wanted = set(HEAP_WRAPPED)
    function = ""
    function_owner = "?"
    real_calls: dict[str, set[str]] = {}
    stray: list[str] = []
    wrapped_calls = 0
    for line in result.stdout.splitlines():
        header = re.match(r"^([0-9a-f]+) <(.*)>:$", line)
        if header:
            function = header.group(2)
            function_owner = owner_of(int(header.group(1), 16))
            continue
        branch = re.search(r"\bbl?\s+[0-9a-f]+ <([^>+]+)>", line)
        if not branch:
            continue
        target = branch.group(1)
        if target.startswith("__wrap_") and target[len("__wrap_"):] in wanted:
            wrapped_calls += 1
        if target not in wanted:
            continue
        if function == f"__wrap_{target}":
            real_calls.setdefault(target, set()).add(function)
        elif not function_owner.endswith("(JKRHeap.o)"):
            stray.append(f"{function} [{function_owner}] -> {target}")
    for symbol in HEAP_WRAPPED:
        if f"__wrap_{symbol}" not in real_calls.get(symbol, set()):
            return heap_fail(f"__wrap_{symbol} does not fall through to Galaxy's {symbol} (__real_)")
    if stray:
        return heap_fail("unwrapped operator new/delete calls outside JKRHeap.o:\n  "
                    + "\n  ".join(sorted(stray)[:40]))
    print(f"heap-wrap: PASS: {len(HEAP_WRAPPED)} operator new/delete symbols wrapped by "
          f"port_heap.o; __real_ = JKRHeap.o; {wrapped_calls} call sites use __wrap_")
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
    parser.add_argument("--objdump", type=Path,
                        help="defaults to the objdump next to --nm")
    parser.add_argument("--audio-elf", type=Path)
    parser.add_argument("--audio-map", type=Path)
    args = parser.parse_args()

    for path in (args.elf, args.map, args.provider_object):
        if not path.is_file():
            return fail(f"missing audit input: {path}")
    objdump = args.objdump or args.nm.with_name(args.nm.name.replace("nm", "objdump"))
    try:
        result = check_heap_wrap(args.nm, objdump, args.elf, args.map)
    except (OSError, subprocess.CalledProcessError) as exc:
        return fail(f"heap-wrap audit failed: {exc}")
    if result:
        return result
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
