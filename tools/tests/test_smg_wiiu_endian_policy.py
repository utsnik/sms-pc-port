#!/usr/bin/env python3
"""Check the executable native-BE/32-bit policy for the Galaxy Wii U port."""

import argparse
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def fail(messages):
    for message in messages:
        print(f"endian-policy: FAIL: {message}")
    return 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--abi-audit", type=Path)
    parser.add_argument("--swap-audit", type=Path)
    args = parser.parse_args()

    files = {
        "compat": ROOT / "games/smg/include/port/compat.h",
        "intrinsics": ROOT / "games/smg/include/port/ppc_intrinsics.h",
        "fifo": ROOT / "games/smg/include/port/gx_fifo.h",
        "string": ROOT / "games/smg/decomp/src/Game/Util/StringUtil.cpp",
        "unit_heap": ROOT / "games/smg/decomp/src/JSystem/JKernel/JKRUnitHeap.cpp",
        "tpl": ROOT / "games/smg/decomp/src/RVL_SDK/tpl/TPL.c",
        "policy": ROOT / "games/smg/platform/wiiu/patch-policy.md",
    }
    errors = [f"missing {name}: {path}" for name, path in files.items() if not path.is_file()]
    if errors:
        return fail(errors)

    text = {name: path.read_text(encoding="utf-8") for name, path in files.items()}
    required = {
        "compat": ("PORT_HOST_BIG_ENDIAN", "__BYTE_ORDER__", "__ORDER_BIG_ENDIAN__"),
        "intrinsics": ("#if PORT_HOST_BIG_ENDIAN", "__lwbrx", "__lhbrx", "__stwbrx", "__sthbrx"),
        # The FIFO writers feed sms_gx's big-endian pipe byte by byte, the
        # stream GXBegin and the GX API write (no separate buffer, no swap).
        "fifo": ("__PortGXFifo_u32", "GXPC_Write32", "GXPC_PipeBuf"),
        "string": ("#if !PORT_HOST_BIG_ENDIAN", "convertUTF16ToASCII"),
        "unit_heap": ("PORT_HOST_BIG_ENDIAN", "__builtin_bswap32"),
        "tpl": ("PORT_HOST_BIG_ENDIAN", "uintptr_t", "descriptorArray"),
        "policy": ("native-BE", "identity/no-op", "guarded-off", "deferred", "PTR32", "uintptr_t"),
    }
    for name, needles in required.items():
        for needle in needles:
            if needle not in text[name]:
                errors.append(f"{name} lacks {needle}")

    for name in ("string", "tpl"):
        if re.search(r"defined\(TARGET_PC\).*PORT_HOST_BIG_ENDIAN", text[name]):
            errors.append(f"{name} still uses TARGET_PC as an endian test")

    if args.abi_audit:
        if not args.abi_audit.is_file():
            errors.append(f"ABI audit is missing: {args.abi_audit}")
        else:
            audit = args.abi_audit.read_text(encoding="utf-8")
            if "PORT_HOST_BIG_ENDIAN" not in audit:
                errors.append("ABI audit did not record PORT_HOST_BIG_ENDIAN")
            for line in audit.splitlines():
                if re.search(r"std::atomic<uint64_t>|thread_local", line) and "games/smg/platform/wiiu" in line and "patch-policy.md:" not in line:
                    errors.append(f"Stage 1 platform contains forbidden 64-bit atomics or TLS: {line}")

    if args.swap_audit:
        if not args.swap_audit.is_file():
            errors.append(f"swap audit is missing: {args.swap_audit}")
        else:
            swap = args.swap_audit.read_text(encoding="utf-8")
            if "__builtin_bswap" in swap:
                for line in swap.splitlines():
                    if "__builtin_bswap" not in line or "patch-policy.md:" in line:
                        continue
                    approved = (
                        "ppc_intrinsics.h",
                        "gx_fifo.h",
                        "JKRUnitHeap.cpp",
                        "platform/wiiu/gx2_renderer.cpp",
                        "platform/wiiu/gx2_shader_inl.h",
                        "platform/gx/src/",
                        "platform/endian/endian_util.h",
                        "platform/mods/eclipse/fixup_sources.py",
                    )
                    if not any(name in line for name in approved):
                        errors.append(f"unclassified direct swap: {line}")

    if errors:
        return fail(errors)

    print("endian-policy: PASS: native-BE boundaries classified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
