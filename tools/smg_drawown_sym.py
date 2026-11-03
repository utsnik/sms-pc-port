#!/usr/bin/env python3
"""Symbolize the last SMG Wii U drawown report from a hardware run log."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
import re
import subprocess
import sys


FRAME_RE = re.compile(rb"\[drawown\] frame=(\d+) total=(\d+)")
OWNER_RE = re.compile(
    rb"\[drawown\]\s+owner=(.*?)\s+"
    rb"(?:vt=([0-9a-fA-F]+)\s+)?draws=(\d+)\s+verts=(\d+)\s+kind=(\S+)"
)
PC_RE = re.compile(
    rb"\[drawown\]\s+pc=([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+"
    rb"draws=(\d+)\s+verts=(\d+)"
)


@dataclass
class Owner:
    raw_name: bytes
    vtable: int | None
    draws: int
    verts: int
    kind: bytes


@dataclass
class Caller:
    pcs: tuple[int, int, int]
    draws: int
    verts: int


@dataclass
class Window:
    frame: int
    total: int
    owners: list[Owner] = field(default_factory=list)
    callers: list[Caller] = field(default_factory=list)


def read_windows(path: Path) -> list[Window]:
    windows: list[Window] = []
    current: Window | None = None
    for line in path.read_bytes().splitlines():
        frame = FRAME_RE.search(line)
        if frame:
            current = Window(int(frame.group(1)), int(frame.group(2)))
            windows.append(current)
            continue
        if current is None:
            continue
        owner = OWNER_RE.search(line)
        if owner:
            current.owners.append(
                Owner(
                    owner.group(1),
                    int(owner.group(2), 16) if owner.group(2) else None,
                    int(owner.group(3)),
                    int(owner.group(4)),
                    owner.group(5),
                )
            )
            continue
        caller = PC_RE.search(line)
        if caller:
            current.callers.append(
                Caller(
                    (int(caller.group(1), 16), int(caller.group(2), 16), int(caller.group(3), 16)),
                    int(caller.group(4)),
                    int(caller.group(5)),
                )
            )
    return windows


def decode_owner(raw: bytes) -> str:
    return raw.decode("cp932", errors="replace")


def docker_tool(elf: Path, image: str, tool: str, args: list[str]) -> str:
    command = [
        "docker",
        "run",
        "--rm",
        "--network",
        "none",
        "-v",
        f"{elf.parent.resolve()}:/e:ro",
        image,
        f"/opt/devkitpro/devkitPPC/bin/{tool}",
        *args,
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip() or "tool failed"
        raise RuntimeError(f"{tool}: {detail}")
    return result.stdout


def symbolize_pcs(elf: Path, image: str, pcs: list[int]) -> dict[int, str]:
    if not pcs:
        return {}
    output = docker_tool(
        elf,
        image,
        "powerpc-eabi-addr2line",
        ["-f", "-C", "-e", f"/e/{elf.name}", *(f"0x{pc:x}" for pc in pcs)],
    )
    lines = output.splitlines()
    symbols: dict[int, str] = {}
    for index, pc in enumerate(pcs):
        function = lines[index * 2] if index * 2 < len(lines) else "??"
        location = lines[index * 2 + 1] if index * 2 + 1 < len(lines) else "??:0"
        symbols[pc] = f"{function} at {location}"
    return symbols


def read_nm(elf: Path, image: str) -> list[tuple[int, str]]:
    output = docker_tool(
        elf,
        image,
        "powerpc-eabi-nm",
        ["-n", "-C", "--defined-only", f"/e/{elf.name}"],
    )
    symbols: list[tuple[int, str]] = []
    for line in output.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3:
            try:
                symbols.append((int(fields[0], 16), fields[2]))
            except ValueError:
                pass
    symbols.sort()
    return symbols


def symbolize_vtable(symbols: list[tuple[int, str]], address: int | None) -> str:
    if address is None:
        return "-"
    exact = [name for value, name in symbols if value == address]
    if exact:
        return exact[0]
    previous = [(value, name) for value, name in symbols if value < address]
    if not previous:
        return "??"
    value, name = previous[-1]
    return f"{name}+0x{address - value:x}"


def print_window(window: Window, pc_symbols: dict[int, str], nm_symbols: list[tuple[int, str]]) -> None:
    print(f"[drawown] frame={window.frame} total={window.total}")
    for owner in window.owners:
        name = decode_owner(owner.raw_name)
        vt = "-" if owner.vtable is None else f"0x{owner.vtable:08x}"
        vt_symbol = symbolize_vtable(nm_symbols, owner.vtable)
        print(
            f"owner={name} vt={vt} draws={owner.draws} verts={owner.verts} "
            f"kind={owner.kind.decode('ascii', errors='replace')} [{vt_symbol}]"
        )
    for caller in window.callers:
        pcs = " ".join(f"0x{pc:08x}" for pc in caller.pcs)
        symbols = " | ".join(pc_symbols.get(pc, "??") for pc in caller.pcs if pc)
        print(f"pc={pcs} draws={caller.draws} verts={caller.verts} [{symbols or 'no caller frame'}]")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_log", type=Path)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--docker-image", default="soh-wiiu-builder:latest")
    args = parser.parse_args()

    if not args.run_log.is_file():
        parser.error(f"run log does not exist: {args.run_log}")
    if not args.elf.is_file():
        parser.error(f"ELF does not exist: {args.elf}")
    windows = read_windows(args.run_log)
    if not windows:
        print("smg_drawown_sym: no [drawown] window found", file=sys.stderr)
        return 1
    window = windows[-1]
    pcs = sorted({pc for caller in window.callers for pc in caller.pcs if pc})
    try:
        pc_symbols = symbolize_pcs(args.elf, args.docker_image, pcs)
        nm_symbols = read_nm(args.elf, args.docker_image)
    except RuntimeError as error:
        print(f"smg_drawown_sym: {error}", file=sys.stderr)
        return 1
    print_window(window, pc_symbols, nm_symbols)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
