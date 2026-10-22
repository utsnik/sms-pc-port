#!/usr/bin/env python3
"""Host test for ruling R17: SMG's port-side operator new/delete routing.

Compiles the shipped platform/wiiu/port_heap.cpp (PORT_GAME_SMG) with stub
coreinit headers and checks that SMS_PORT_HEAP_SCOPE allocations go to the
system heap, unscoped ones to the game (__real_) allocator, and that delete
pairs by tag. Also checks port_host_alloc's alignment rule.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT_HEAP = ROOT / "platform/wiiu/port_heap.cpp"
SMG_WIIU = ROOT / "games/smg/platform/wiiu"
HARNESS = Path(__file__).with_name("smg_wiiu_port_heap_host.cpp")

MEMDEFAULTHEAP = """#pragma once
#include <stdint.h>
void* MEMAllocFromDefaultHeapEx(uint32_t size, int alignment);
void MEMFreeToDefaultHeap(void* p);
"""

THREAD = """#pragma once
typedef enum OSThreadSpecificID { OS_THREAD_SPECIFIC_13 = 13 } OSThreadSpecificID;
void* OSGetThreadSpecific(OSThreadSpecificID id);
void OSSetThreadSpecific(OSThreadSpecificID id, void* value);
"""


def fail(message: str) -> int:
    print(f"port-heap: FAIL: {message}")
    return 1


def main() -> int:
    runtime = (SMG_WIIU / "runtime.cpp").read_text(encoding="utf-8")
    if "s_alloc_scope_depth" in runtime or "port_host_alloc_scope" in runtime:
        return fail("runtime.cpp still carries the dead PortHostAllocScope machinery")
    if "memalign(smg_wiiu::host_alloc_alignment(" not in runtime:
        return fail("port_host_alloc does not memalign with host_alloc_alignment()")
    with tempfile.TemporaryDirectory(prefix="smg-port-heap-") as build:
        stub = Path(build) / "include" / "coreinit"
        stub.mkdir(parents=True)
        (stub / "memdefaultheap.h").write_text(MEMDEFAULTHEAP, encoding="utf-8")
        (stub / "thread.h").write_text(THREAD, encoding="utf-8")
        exe = Path(build) / "port_heap_host"
        compile_result = subprocess.run(
            ["g++", "-std=gnu++11", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-D__WIIU__=1", "-DPORT_GAME_SMG=1",
             "-I", str(Path(build) / "include"), "-I", str(PORT_HEAP.parent),
             "-I", str(SMG_WIIU),
             str(HARNESS), str(PORT_HEAP), "-o", str(exe)],
            capture_output=True, text=True)
        if compile_result.returncode:
            return fail("compile/link failed:\n" + (compile_result.stdout + compile_result.stderr).rstrip())
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        output = (run.stdout + run.stderr).rstrip()
        if run.returncode:
            return fail(output)
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
