#!/usr/bin/env python3
"""Check that SMG loads the three native-BE objects from sys/main.dol."""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path


ERROR_NAME = "layoutdata/errormessagewindow.arc"
TABLES = {
    "error": (2, ("name", "progress")),
    "galaxy": (7, ("name", "MapPaneName", "PowerStarNum")),
}


def fail(message: str) -> int:
    print(f"dol-data: FAIL: {message}")
    return 1


def be16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def jmap_hash(name: str) -> int:
    result = 0
    for byte in name.encode():
        signed = byte - 256 if byte >= 128 else byte
        result = (signed + result * 31) & 0xFFFFFFFF
    return result


def cstring_end(data: bytes, offset: int, limit: int) -> int | None:
    if offset < 0 or offset >= limit:
        return None
    end = data.find(b"\0", offset, limit)
    return end + 1 if end >= 0 else None


def find_rarc(data: bytes) -> tuple[int, int] | None:
    for offset in range(0, max(0, len(data) - 0x40 + 1)):
        if data[offset : offset + 4] != b"RARC":
            continue
        size = be32(data, offset + 4)
        if size < 0x40 or offset + size > len(data):
            continue
        info = offset + 0x20
        if info + 0x18 > offset + size:
            continue
        node_count = be32(data, info)
        node_offset = be32(data, info + 4) + offset + 0x20
        entry_count = be32(data, info + 8)
        entry_offset = be32(data, info + 12) + offset + 0x20
        string_size = be32(data, info + 16)
        string_offset = be32(data, info + 20) + offset + 0x20
        end = offset + size
        if not node_count or node_count > 4096 or entry_count > 65536:
            continue
        if node_offset + node_count * 0x10 > end:
            continue
        if entry_offset + entry_count * 0x14 > end:
            continue
        if string_offset + string_size > end:
            continue

        def string_at(name_offset: int) -> bytes | None:
            if name_offset >= string_size:
                return None
            string_end = cstring_end(data, string_offset + name_offset, string_offset + string_size)
            if string_end is None:
                return None
            return data[string_offset + name_offset : string_end - 1]

        nodes = []
        valid = True
        for index in range(node_count):
            node = node_offset + index * 0x10
            name = string_at(be32(data, node + 4))
            entries = be16(data, node + 10)
            first = be32(data, node + 12)
            if name is None or first > entry_count or entries > entry_count - first:
                valid = False
                break
            nodes.append((name, entries, first))
        if not valid:
            continue

        def walk(node_index: int, prefix: str, seen: set[int]):
            if node_index >= len(nodes) or node_index in seen:
                return
            seen.add(node_index)
            _, count, first = nodes[node_index]
            for index in range(first, first + count):
                entry = entry_offset + index * 0x14
                name = string_at(be16(data, entry + 6))
                if name is None:
                    return
                flags = be16(data, entry + 4)
                child_or_offset = be32(data, entry + 8)
                file_size = be32(data, entry + 12)
                if flags & 0x0200:
                    if name not in (b".", b".."):
                        yield from walk(child_or_offset, prefix + name.decode("latin1") + "/", seen)
                else:
                    if child_or_offset > size or file_size > size - child_or_offset:
                        return
                    yield prefix + name.decode("latin1"), (
                        offset + 0x20 + be32(data, offset + 12) + child_or_offset,
                        file_size,
                    )

        for name, (file_offset, file_size) in walk(0, "", set()) or ():
            if name.lower().endswith(ERROR_NAME):
                return offset, size
    return None


def find_table(data: bytes, wanted_names: tuple[str, ...], field_count: int) -> tuple[int, int] | None:
    wanted = {jmap_hash(name) for name in wanted_names}
    first_hash = struct.pack(">I", jmap_hash(wanted_names[0]))
    hit = data.find(first_hash)
    while hit >= 0:
        for column in range(field_count):
            position = hit - 16 - 12 * column
            if position < 0 or position + 16 + 12 * field_count > len(data):
                continue
            rows = struct.unpack_from(">i", data, position)[0]
            fields = struct.unpack_from(">i", data, position + 4)[0]
            data_offset = struct.unpack_from(">i", data, position + 8)[0]
            entry_size = be32(data, position + 12)
            if not 0 < rows < 4096 or fields != field_count:
                continue
            if data_offset != 16 + 12 * fields or not 0 < entry_size <= 1024:
                continue
            if data_offset > len(data) - position or rows > (len(data) - position - data_offset) // entry_size:
                continue
            descriptors = []
            valid = True
            for index in range(fields):
                descriptor = position + 16 + 12 * index
                field_offset = be16(data, descriptor + 8)
                field_type = data[descriptor + 11]
                if field_offset >= entry_size or (field_type == 6 and field_offset + 4 > entry_size):
                    valid = False
                    break
                descriptors.append((be32(data, descriptor), field_offset, field_type))
            if not valid or wanted - {item[0] for item in descriptors}:
                continue
            string_pool = position + data_offset + rows * entry_size
            if string_pool > len(data):
                continue
            end = string_pool
            for row in range(rows):
                entry = position + data_offset + row * entry_size
                for _, field_offset, field_type in descriptors:
                    if field_type != 6:
                        continue
                    string_offset = be32(data, entry + field_offset)
                    string_end = cstring_end(data, string_pool + string_offset, len(data))
                    if string_end is None:
                        valid = False
                        break
                    end = max(end, string_end)
                if not valid:
                    break
            if not valid:
                continue
            size = (end - position + 31) & ~31
            if size <= 0 or position + size > len(data):
                continue
            return position, size
        hit = data.find(first_hash, hit + 1)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--expect-error", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    parser.add_argument("--expect-story", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    parser.add_argument("--expect-galaxy", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    args = parser.parse_args()

    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError:
        return fail("missing main.dol loader source")
    try:
        dol = args.dol.read_bytes()
    except OSError as exc:
        return fail(str(exc))

    source_dir = args.source.parent
    stubs = source_dir / "galaxy_link_stubs.cpp"
    bcsv_include = args.source.parents[2] / "decomp/src/Game/System/BCSVInclude.s"
    if not re.search(r"sms_wiiu_dvd_read_file\s*\(\s*\"/sys/main\.dol\"", source):
        return fail("missing main.dol loader through the Task 2 DVD boundary")
    if "native-BE" not in source or "alignas(32)" not in source:
        return fail("loader does not document native-BE aligned target storage")
    if "ErrorMessageArchive.arc" in source or "StoryEvent.bcsv" in source or "GalaxyID.bcsv" in source:
        return fail("loader still loads cooked sys/*.arc or sys/*.bcsv files")
    try:
        stubs_source = stubs.read_text(encoding="utf-8")
        bcsv_source = bcsv_include.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(str(exc))
    if re.search(r"(?:cErrorArchive|StoryEventBCSV|GalaxyIDBCSV)\s*\[[^]]*1[^]]*\]", stubs_source):
        return fail("Stage 1 one-byte table placeholder remains")
    if ".incbin" in bcsv_source:
        return fail("BCSVInclude.s still has a cooked-file .incbin provider")

    archive = find_rarc(dol)
    story = find_table(dol, TABLES["error"][1], TABLES["error"][0])
    galaxy = find_table(dol, TABLES["galaxy"][1], TABLES["galaxy"][0])
    if archive is None:
        return fail("native-BE ErrorMessageArchive RARC not found")
    if story is None:
        return fail("native-BE StoryEvent BCSV not found")
    if galaxy is None:
        return fail("native-BE GalaxyID BCSV not found")
    expected = (args.expect_error, args.expect_story, args.expect_galaxy)
    actual = (archive, story, galaxy)
    for name, got, want in zip(("ErrorMessageArchive", "StoryEvent", "GalaxyID"), actual, expected):
        if want is not None and got != want:
            return fail(f"{name}=0x{got[0]:x}:{got[1]}, expected 0x{want[0]:x}:{want[1]}")
    print(
        "dol-data: PASS: "
        f"ErrorMessageArchive={archive[1]} StoryEvent={story[1]} GalaxyID={galaxy[1]}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
