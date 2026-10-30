#!/usr/bin/env python3
"""Build the optional single-file SMG data pack used by the Wii U DVD reader."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


DEFAULT_SOURCE = Path("/mnt/build2/wiiu-build/gamedata/smg/extracted/DATA/files")
DEFAULT_OUTPUT = Path("/mnt/build2/wiiu-build/gamedata/smg/pack/pack.bin")
MAGIC = b"SMGP"
VERSION = 1
HEADER_SIZE = 32
ALIGNMENT = 64
MAX_FILE_SIZE = 4 * 1024 * 1024
COPY_BUFFER_SIZE = 1024 * 1024


@dataclass(frozen=True)
class PackFile:
    relative_path: Path
    source_path: Path
    name: bytes
    size: int


def aligned(value: int) -> int:
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1)


def included(relative_path: Path, size: int) -> bool:
    parts = relative_path.parts
    if size >= MAX_FILE_SIZE or not parts:
        return False
    if parts[0] == "MovieData":
        return False
    return not (parts[0] == "AudioRes" and len(parts) > 1 and parts[1].startswith("Stream"))


def collect(source: Path) -> list[PackFile]:
    files: list[PackFile] = []
    for path in sorted(source.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(source)
        size = path.stat().st_size
        if not included(relative, size):
            continue
        name = ("/" + relative.as_posix()).encode("utf-8")
        if len(name) > 0xFFFF:
            raise ValueError(f"pack path is too long: {relative}")
        files.append(PackFile(relative, path, name, size))
    return files


def build(source: Path, output: Path) -> tuple[int, int]:
    files = collect(source)
    index_size = sum(12 + len(item.name) for item in files)
    data_offset = aligned(HEADER_SIZE + index_size)
    offsets: list[int] = []
    current = data_offset
    for item in files:
        offsets.append(current)
        current = aligned(current + item.size)

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as packed:
        packed.write(struct.pack(">4s7I", MAGIC, VERSION, len(files), index_size,
                                 data_offset, 0, 0, 0))
        for item, offset in zip(files, offsets):
            packed.write(struct.pack(">HH", len(item.name), 0))
            packed.write(item.name)
            packed.write(struct.pack(">II", offset, item.size))
        packed.write(b"\0" * (data_offset - packed.tell()))
        for item, offset in zip(files, offsets):
            if packed.tell() != offset:
                raise AssertionError(f"bad pack alignment before {item.relative_path}")
            with item.source_path.open("rb") as source_file:
                remaining = item.size
                while remaining:
                    chunk = source_file.read(min(remaining, COPY_BUFFER_SIZE))
                    if not chunk:
                        raise OSError(f"short read while packing {item.source_path}")
                    packed.write(chunk)
                    remaining -= len(chunk)
            padding = aligned(item.size) - item.size
            if padding:
                packed.write(b"\0" * padding)
    return len(files), output.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    count, size = build(args.source, args.output)
    print(f"packed {count} files, {size} bytes: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
