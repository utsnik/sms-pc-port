#!/usr/bin/env python3
"""Check the raw native-Espresso-BE RMGP01 extraction used by Stage 2."""

import argparse
import re
import struct
import sys
from pathlib import Path, PurePosixPath


EXPECTED_FST = (2465, 2380, 85)
EXPECTED_DOL = {
    "ErrorMessageArchive": (0x53A0E0, 39616),
    "StoryEvent": (0x538720, 480),
    "GalaxyID": (0x538900, 3360),
}
SCENE_PATH = "StageData/AstroDome/AstroDomeScenario.arc"
SCENE_BYTES = 448


def be_u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def jmap_hash(name):
    value = 0
    for char in name.encode("ascii"):
        if char >= 128:
            char -= 256
        value = (char + value * 31) & 0xFFFFFFFF
    return value


def parse_compare_dol(value):
    if not value:
        return {}
    result = {}
    for item in value.split(","):
        try:
            offset, size = item.split(":", 1)
            offset, size = int(offset, 0), int(size, 0)
            if offset in result:
                raise ValueError("duplicate offset")
            result[offset] = size
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                "--compare-dol entries must be OFFSET:SIZE"
            ) from exc
    return result


def rejected_fixture_path(path):
    """Reject cooked/tool fixture roots instead of silently accepting them."""
    components = {part.lower() for part in path.resolve().parts}
    if {"tools", "cook"} & components:
        return "fixture path contains a tools/cook directory"
    for part in components:
        if re.fullmatch(r"little[-_]?endian(?:[-_].*)?", part):
            return "fixture path contains a little-endian directory"
    return None


def cstring(data, offset):
    if offset < 0 or offset >= len(data):
        raise ValueError("string offset outside buffer")
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated FST string")
    return data[offset:end].decode("ascii")


def parse_fst(fst):
    if len(fst) < 12:
        raise ValueError("FST is shorter than its root entry")
    root_word, root_parent, count = struct.unpack_from(">III", fst, 0)
    if root_word != 0x01000000 or root_parent != 0:
        raise ValueError("invalid FST root entry")
    if count <= 0 or count * 12 > len(fst):
        raise ValueError("FST entry table is outside fst.bin")
    string_base = count * 12
    entries = []
    for index in range(count):
        word, value, third = struct.unpack_from(">III", fst, index * 12)
        is_dir = (word >> 24) == 1
        name_offset = word & 0x00FFFFFF
        name = "" if index == 0 else cstring(fst, string_base + name_offset)
        if index and (not name or "/" in name or "\\" in name):
            raise ValueError("invalid FST entry name at index %d" % index)
        if is_dir:
            if third <= index or third > count:
                raise ValueError("invalid FST directory range at index %d" % index)
            if value >= index and index != 0:
                raise ValueError("invalid FST directory parent at index %d" % index)
        entries.append((is_dir, value, third, name))

    paths = [""] * count
    stack = [(0, count, "")]
    for index in range(1, count):
        while stack and index >= stack[-1][1]:
            stack.pop()
        if not stack:
            raise ValueError("FST entry %d is outside the root directory" % index)
        parent_path = stack[-1][2]
        name = entries[index][3]
        paths[index] = "%s/%s" % (parent_path, name) if parent_path else name
        if entries[index][0]:
            if entries[index][1] != stack[-1][0]:
                raise ValueError("FST directory parent mismatch at index %d" % index)
            stack.append((index, entries[index][2], paths[index]))
    return entries, paths


def safe_fixture_path(files_root, relative):
    pure = PurePosixPath(relative)
    if pure.is_absolute() or any(part in ("", ".", "..") for part in pure.parts):
        raise ValueError("FST path is not a safe relative path: %s" % relative)
    candidate = (files_root / Path(*pure.parts)).resolve()
    try:
        candidate.relative_to(files_root)
    except ValueError as exc:
        raise ValueError("FST path escapes DATA/files: %s" % relative) from exc
    return candidate


def yaz0_decompress(data):
    if data[:4] != b"Yaz0":
        raise ValueError("sample does not have a Yaz0 header")
    output_size = be_u32(data, 4)
    if output_size == 0:
        raise ValueError("Yaz0 stream has an empty output")
    output = bytearray(output_size)
    source = 16
    destination = 0
    while destination < output_size:
        if source >= len(data):
            raise ValueError("truncated Yaz0 code byte")
        code = data[source]
        source += 1
        for bit in range(8):
            if destination >= output_size:
                break
            if code & (0x80 >> bit):
                if source >= len(data):
                    raise ValueError("truncated Yaz0 literal")
                output[destination] = data[source]
                source += 1
                destination += 1
                continue
            if source + 1 >= len(data):
                raise ValueError("truncated Yaz0 back-reference")
            first, second = data[source], data[source + 1]
            source += 2
            distance = ((first & 0x0F) << 8) | second
            copy_source = destination - distance - 1
            if copy_source < 0:
                raise ValueError("invalid Yaz0 back-reference")
            length = first >> 4
            if length == 0:
                if source >= len(data):
                    raise ValueError("truncated Yaz0 long length")
                length = data[source] + 0x12
                source += 1
            else:
                length += 2
            if destination + length > output_size:
                raise ValueError("Yaz0 output exceeds declared size")
            for _ in range(length):
                output[destination] = output[copy_source]
                destination += 1
                copy_source += 1
    return bytes(output)


def rarc_has_error_window(data):
    if data[:4] != b"RARC" or len(data) < 0x44:
        return False
    data_offset = be_u32(data, 0x0C) + 0x20
    node_count, node_offset, entry_count, entry_offset, string_size, string_offset = struct.unpack_from(
        ">IIIIII", data, 0x20
    )
    del string_size
    node_offset += 0x20
    entry_offset += 0x20
    string_offset += 0x20
    if data_offset > len(data) or node_offset + node_count * 0x10 > len(data):
        return False
    if entry_offset + entry_count * 0x14 > len(data):
        return False

    def name_at(offset):
        return cstring(data, string_offset + offset)

    nodes = []
    try:
        for index in range(node_count):
            _, name_offset, _, entry_count_for_node, first_entry = struct.unpack_from(
                ">IIHHI", data, node_offset + index * 0x10
            )
            if first_entry + entry_count_for_node > entry_count:
                return False
            nodes.append((name_at(name_offset), entry_count_for_node, first_entry))

        def walk(node_index, prefix, active):
            if node_index >= len(nodes) or node_index in active:
                return False
            name, count, first = nodes[node_index]
            active = active | {node_index}
            for entry_index in range(first, first + count):
                _, _, flags_type, name_offset, data_offset_or_node, size = struct.unpack_from(
                    ">HHHHII", data, entry_offset + entry_index * 0x14
                )
                entry_name = name_at(name_offset)
                if flags_type >> 8 & 0x02:
                    if entry_name in (".", ".."):
                        continue
                    if walk(data_offset_or_node, prefix + entry_name + "/", active):
                        return True
                elif (prefix + entry_name).lower() == "layoutdata/errormessagewindow.arc":
                    return True
            return False

        # The first node is the archive root; GalaxyQuest's rule compares
        # paths below that node (for example, layoutdata/foo.arc).
        return walk(0, "", set())
    except (IndexError, UnicodeDecodeError, ValueError, struct.error):
        return False


def find_error_archive(dol):
    position = dol.find(b"RARC")
    while position >= 0:
        if position + 8 <= len(dol):
            size = be_u32(dol, position + 4)
            if 0x40 <= size <= len(dol) - position:
                archive = dol[position : position + size]
                if rarc_has_error_window(archive):
                    return position, size
        position = dol.find(b"RARC", position + 4)
    return None


def table_at(dol, position, columns, wanted):
    if position < 0 or position + 16 + 12 * columns > len(dol):
        return None
    rows, fields, data_offset, entry_size = struct.unpack_from(">iiii", dol, position)
    if (
        fields != columns
        or data_offset != 16 + 12 * fields
        or not 0 < rows < 4096
        or not 0 < entry_size <= 1024
    ):
        return None
    descriptors = [
        struct.unpack_from(">IIHBB", dol, position + 16 + 12 * index)
        for index in range(fields)
    ]
    if not wanted <= {descriptor[0] for descriptor in descriptors}:
        return None
    strings = position + data_offset + rows * entry_size
    if strings > len(dol):
        return None
    end = strings
    try:
        for row in range(rows):
            for _, _, field_offset, _, kind in descriptors:
                if kind == 6:
                    value_offset = be_u32(dol, position + data_offset + row * entry_size + field_offset)
                    string_at = strings + value_offset
                    end = max(end, dol.index(b"\0", string_at) + 1)
    except (ValueError, struct.error):
        return None
    end = position + ((end - position + 31) & ~31)
    return min(end, len(dol))


def find_table(dol, columns, names):
    wanted = {jmap_hash(name) for name in names}
    key = struct.pack(">I", jmap_hash(names[0]))
    hit = dol.find(key)
    while hit >= 0:
        for index in range(columns):
            end = table_at(dol, hit - 16 - 12 * index, columns, wanted)
            if end is not None:
                return hit - 16 - 12 * index, end - (hit - 16 - 12 * index)
        hit = dol.find(key, hit + 1)
    return None


def fail(messages):
    for message in messages:
        print("retail-BE FST: FAIL: %s" % message)
    print("retail-BE FST: %d failures" % len(messages))
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("iso", type=Path)
    parser.add_argument("data", type=Path)
    parser.add_argument("--compare-dol", type=parse_compare_dol, default={})
    args = parser.parse_args(argv)
    failures = []

    if not args.iso.is_file():
        failures.append("ISO is not a readable file: %s" % args.iso)
    if not args.data.is_dir():
        failures.append("DATA is not a directory: %s" % args.data)
    if failures:
        return fail(failures)

    data_root = args.data.resolve()
    rejected = rejected_fixture_path(data_root)
    if rejected:
        return fail([rejected])
    files_root = (data_root / "files").resolve()
    if not files_root.is_dir():
        return fail(["missing DATA/files directory"])
    rejected = rejected_fixture_path(files_root)
    if rejected:
        return fail([rejected])

    try:
        boot = (data_root / "sys/boot.bin").read_bytes()
        fst = (data_root / "sys/fst.bin").read_bytes()
        dol = (data_root / "sys/main.dol").read_bytes()
        entries, paths = parse_fst(fst)
    except (OSError, ValueError, struct.error) as exc:
        return fail([str(exc)])

    files = sum(not entry[0] for entry in entries)
    directories = sum(entry[0] for entry in entries)
    if (len(entries), files, directories) != EXPECTED_FST:
        failures.append(
            "FST counts are %d/%d/%d, expected %d/%d/%d"
            % ((len(entries), files, directories) + EXPECTED_FST)
        )

    mapped = {}
    for index, (is_dir, _, size, _) in enumerate(entries[1:], 1):
        try:
            path = safe_fixture_path(files_root, paths[index])
            if is_dir:
                if not path.is_dir():
                    failures.append("missing FST directory: %s" % paths[index])
            else:
                if not path.is_file():
                    failures.append("missing FST file: %s" % paths[index])
                elif path.stat().st_size != size:
                    failures.append(
                        "size mismatch for %s: %d, expected %d"
                        % (paths[index], path.stat().st_size, size)
                    )
            mapped[paths[index].lower()] = path
        except (OSError, ValueError) as exc:
            failures.append(str(exc))
    if SCENE_PATH.lower() not in mapped:
        failures.append("representative scene is absent from the FST: %s" % SCENE_PATH)

    if len(boot) != 1088 or boot[:6] != b"RMGP01":
        failures.append("boot.bin is not the 1088-byte RMGP01 boot read")
    scene = mapped.get(SCENE_PATH.lower())
    scene_bytes = b""
    if scene is not None and scene.is_file():
        scene_bytes = scene.read_bytes()
        if len(scene_bytes) != SCENE_BYTES:
            failures.append("scene read is %d bytes, expected %d" % (len(scene_bytes), SCENE_BYTES))

    yaz0_sample = None
    for index, (is_dir, _, _, _) in enumerate(entries[1:], 1):
        if is_dir:
            continue
        path = mapped.get(paths[index].lower())
        try:
            with path.open("rb") as stream:
                header = stream.read(16)
            if header[:4] == b"Yaz0":
                yaz0_sample = path.read_bytes()
                break
        except OSError as exc:
            failures.append(str(exc))
    if yaz0_sample is None:
        failures.append("no Yaz0 header found in raw FST files")
    else:
        try:
            yaz0_decompress(yaz0_sample)
        except (ValueError, struct.error) as exc:
            failures.append("Yaz0 sample failed: %s" % exc)

    discovered = {
        "ErrorMessageArchive": find_error_archive(dol),
        "StoryEvent": find_table(dol, 2, ("name", "progress")),
        "GalaxyID": find_table(dol, 7, ("name", "MapPaneName", "PowerStarNum")),
    }
    for name, expected in EXPECTED_DOL.items():
        actual = discovered[name]
        if actual != expected:
            failures.append("DOL %s discovered %r, expected %r" % (name, actual, expected))
    if args.compare_dol:
        verified_dol = {offset: size for offset, size in EXPECTED_DOL.values()}
        if args.compare_dol != verified_dol:
            failures.append("--compare-dol disagrees with verified DOL objects")

    if failures:
        return fail(failures)

    print("RMGP01: FST entries=%d files=%d directories=%d" % (len(entries), files, directories))
    print("boot read: %d bytes; scene read: %d bytes" % (len(boot), len(scene_bytes)))
    print("native-BE Yaz0: pass")
    for name, (offset, size) in discovered.items():
        print(
            "DOL discovered %s: offset=%#x bytes=%d expected=PASS"
            % (name, offset, size)
        )
    print("retail-BE FST: 0 failures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
