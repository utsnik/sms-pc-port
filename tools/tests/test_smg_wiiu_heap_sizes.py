#!/usr/bin/env python3
"""Galaxy's fixed heaps must keep their retail sizes on the 32-bit Wii U.

HeapMemoryWatcher::createHeaps scales its fixed heaps with PORT_HEAP(), which
doubles them for GalaxyQuest's 64-bit PC port (128 MiB banks). The Wii U build
defines TARGET_PC too, so the doubled stationed heaps (2 x 0x900000 MEM1,
2 x 0x1400000 MEM2) left the game heaps too small for the scene's 0x1040400
file cache: OSPanic in GameSystemSceneController::initializeScene.

Check the linked ELF: createHeaps loads the retail sizes (0x1E0000 audio solid
heap, 0x900000 stationed MEM1, 0x1400000 stationed GDDR3) and not the doubled
ones (0x3C0000, 0x1200000, 0x2800000).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJDUMP = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump"
FUNC = "_ZN17HeapMemoryWatcher11createHeapsEv"
RETAIL = {"audio": 0x1E0000, "stationed MEM1": 0x900000, "stationed GDDR3": 0x1400000}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, required=True)
    args = parser.parse_args()
    elf = args.elf.resolve()
    text = subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "-v", f"{elf.parent}:/e:ro",
         BUILDER_IMAGE, OBJDUMP, "-d", f"/e/{elf.name}"],
        check=True, capture_output=True, text=True).stdout
    start = text.find(f"<{FUNC}>:\n")
    if start < 0:
        print("heap-sizes: FAIL: HeapMemoryWatcher::createHeaps not found")
        return 1
    body = text[start:text.find("\n\n", start)]
    # All three sizes are multiples of 0x10000: the compiler loads them with lis.
    loaded = {int(m.group(1)) << 16 for m in re.finditer(r"\blis\s+r\d+,(\d+)\b", body)}
    for name, size in RETAIL.items():
        if size not in loaded:
            print(f"heap-sizes: FAIL: {name} heap is not the retail 0x{size:x} (PORT_HEAP doubled?)")
            return 1
        if size * 2 in loaded:
            print(f"heap-sizes: FAIL: doubled {name} heap 0x{size * 2:x} still loaded")
            return 1
    print("heap-sizes: PASS: createHeaps uses the retail Wii heap sizes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
