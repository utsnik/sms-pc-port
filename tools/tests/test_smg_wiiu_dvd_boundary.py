#!/usr/bin/env python3
"""Check the retail FST boundary and the single Wii U DVD provider."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"dvd-boundary: FAIL: {message}")
    return 1


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def source_number(source: str, name: str):
    match = re.search(rf"\b{name}\s*=\s*([0-9]+)u?\s*;", source)
    return int(match.group(1)) if match else None


def check_fst(path: Path, entries: int, files: int, directories: int):
    try:
        data = path.read_bytes()
    except OSError as exc:
        return f"cannot read FST: {exc}"
    if len(data) < 12:
        return "FST is shorter than its header"
    actual_entries = be32(data, 8)
    if actual_entries != entries:
        return f"entries={actual_entries}, expected {entries}"
    table_end = actual_entries * 12
    if table_end > len(data):
        return "FST entry table exceeds the file"

    strings = data[table_end:]
    actual_files = 0
    actual_directories = 1
    paths = ["/"]
    next_ranges = [(0, actual_entries)]
    for index in range(actual_entries):
        word0 = be32(data, index * 12)
        word1 = be32(data, index * 12 + 4)
        word2 = be32(data, index * 12 + 8)
        is_directory = bool(word0 & 0x01000000)
        name_offset = word0 & 0x00FFFFFF
        if index == 0:
            if not is_directory or word1 != 0 or word2 != actual_entries:
                return "invalid FST root entry"
            continue
        if name_offset >= len(strings):
            return f"name offset out of range at entry {index}"
        end = strings.find(b"\0", name_offset)
        if end < 0:
            return f"unterminated name at entry {index}"
        name = strings[name_offset:end]
        if not name or b"/" in name or b"\\" in name or name in (b".", b".."):
            return f"malformed name at entry {index}"

        parent = 0
        for candidate, (_, candidate_next) in enumerate(next_ranges):
            if candidate and index < candidate_next:
                parent = candidate
        if parent >= index or parent >= len(paths):
            return f"invalid parent at entry {index}"
        decoded_name = name.decode("latin1")
        paths.append(paths[parent].rstrip("/") + "/" + decoded_name)
        if is_directory:
            actual_directories += 1
            if word1 >= index or word2 <= index or word2 > actual_entries:
                return f"invalid directory range at entry {index}"
            next_ranges.append((index, word2))
        else:
            actual_files += 1
    if actual_files != files or actual_directories != directories:
        return (
            f"files={actual_files} directories={actual_directories}, "
            f"expected {files}/{directories}"
        )
    return None


def check_compile_db(path: Path) -> str | None:
    try:
        commands = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return f"cannot read compile database: {exc}"
    dvd_sources = []
    for item in commands:
        text = " ".join(str(item.get(key, "")) for key in ("file", "command", "arguments"))
        normalized = text.replace("\\", "/")
        source_file = str(item.get("file", "")).replace("\\", "/")
        if source_file.endswith("/platform/wiiu/dvd.cpp"):
            dvd_sources.append(source_file)
        if "platform/disc/gcdisc.cpp" in normalized:
            return "compile database selects platform/disc/gcdisc.cpp"
        if re.search(r"(?:^|[ /])pread(?:[ (]|$)", text):
            return "compile database selects pread"
        if "/platform/dvd/dvd.cpp" in normalized:
            return "compile database selects the alternate host DVD reader"
    if len(dvd_sources) != 1:
        return f"expected one Wii U DVD provider, found {len(dvd_sources)}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--fst", type=Path, required=True)
    parser.add_argument("--expected-entries", type=int, required=True)
    parser.add_argument("--expected-files", type=int, required=True)
    parser.add_argument("--expected-directories", type=int, required=True)
    parser.add_argument("--compile-db", type=Path)
    args = parser.parse_args()

    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(f"cannot read source: {exc}")

    fst_error = check_fst(
        args.fst, args.expected_entries, args.expected_files, args.expected_directories
    )
    if fst_error:
        return fail(fst_error)

    max_entries = source_number(source, "kMaxEntries")
    max_fst_bytes = source_number(source, "kMaxFstBytes")
    if max_entries is None or max_entries < args.expected_entries:
        return fail(
            f"target FST storage still has the 512-entry Stage 1 limit (need {args.expected_entries})"
        )
    if max_fst_bytes is None or max_fst_bytes < args.fst.stat().st_size:
        return fail(
            f"target FST storage still has the 64 KiB Stage 1 limit (need {args.fst.stat().st_size} bytes)"
        )
    if source_number(source, "kMaxFiles") != 96 or source_number(source, "kMaxPending") != 96:
        return fail("the 96 concurrent file/pending slots were not preserved")
    required_fragments = (
        "sms_wiiu_app_root()",
        "sys/fst.bin",
        "sys/boot.bin",
        "sms_wiiu_dvd_pump",
        "be32(raw)",
        "be32(raw + 4)",
        "be32(raw + 8)",
    )
    missing = [fragment for fragment in required_fragments if fragment not in source]
    if missing:
        return fail("missing Wii U DVD boundary requirement: " + ", ".join(missing))
    if "gcdisc" in source or "pread" in source:
        return fail("source contains a second host DVD provider")
    if args.compile_db:
        compile_error = check_compile_db(args.compile_db)
        if compile_error:
            return fail(compile_error)
    print(
        "dvd-boundary: PASS: "
        f"entries={args.expected_entries} files={args.expected_files} directories={args.expected_directories}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
