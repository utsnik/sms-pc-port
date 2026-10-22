#!/usr/bin/env python3
"""Exercise the Galaxy Stage 2 entry boundary and marker-gated logging."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def fail(message: str) -> int:
    print(f"entry-boundary: FAIL: {message}")
    return 1


HOST_TYPES = r"""
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef float f32;
typedef double f64;
typedef float Mtx[3][4];
typedef float Mtx44[4][4];
typedef struct Vec { f32 x, y, z; } Vec;
typedef struct Quaternion { f32 x, y, z, w; } Quaternion;
"""

HOST_MTX = r"""
#pragma once
#include <revolution/types.h>
extern "C" void PSMTXMultVecSR(const Mtx, const Vec*, Vec*);
"""

HOST_TIME = r"""
#pragma once
#include <stdint.h>
typedef int64_t OSTime;
extern "C" OSTime OSGetTime(void);
"""

HOST_LOG = r"""
#pragma once
extern "C" void WHBLogPrintf(const char*, ...);
"""

HOST_PROC = r"""
#pragma once
extern "C" void WHBProcStopRunning(void);
"""

HOST_HARNESS = r"""
#include "runtime.h"
#include <coreinit/time.h>
#include <revolution/mtx.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

extern "C" void WHBLogPrintf(const char*, ...) {}
extern "C" void WHBProcStopRunning(void) {}
extern "C" OSTime OSGetTime(void) { return 0; }
extern "C" void PSMTXMultVecSR(const Mtx, const Vec* source, Vec* target) {
    *target = *source;
}

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "entry-host: FAIL: %s\n", message);
        std::exit(1);
    }
}

static bool has_open_log_fd(const std::filesystem::path& expected) {
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code error;
        const auto target = std::filesystem::read_symlink(entry.path(), error);
        if (!error && target == expected)
            return true;
    }
    return false;
}

int main(int argc, char** argv) {
    check(argc == 2, "usage");
    const std::filesystem::path root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    check(chdir(root.c_str()) == 0, "chdir");

    const std::filesystem::path app =
        root / "fs:" / "vol" / "external01" / "wiiu" / "apps" / "smg";
    std::filesystem::create_directories(app);
    const std::filesystem::path log = app / "stage2.log";
    const std::filesystem::path marker = app / "stage2log";

    port_log("without marker");
    check(!std::filesystem::exists(log), "stage2.log must be marker-gated");

    std::ofstream(marker).put('\n');
    port_log("first line");
    check(std::filesystem::exists(log), "marker must enable stage2.log");
    {
        std::ifstream input(log);
        std::string content((std::istreambuf_iterator<char>(input)), {});
        check(content == "first line\n", "first log write must be flushed");
    }
    check(!has_open_log_fd(log), "stage2.log must not remain open");

    port_log("second line\n");
    {
        std::ifstream input(log);
        std::string content((std::istreambuf_iterator<char>(input)), {});
        check(content == "first line\nsecond line\n", "append writes must be complete");
    }
    check(!has_open_log_fd(log), "stage2.log must close after every write");
    std::puts("entry-host: PASS: marker gate, flush, append, and close");
    return 0;
}
"""


def check_host_logging(runtime: Path) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="smg-entry-") as temporary:
        temporary_path = Path(temporary)
        include = temporary_path / "include"
        (include / "revolution").mkdir(parents=True)
        (include / "coreinit").mkdir()
        (include / "whb").mkdir()
        (include / "revolution" / "types.h").write_text(HOST_TYPES)
        (include / "revolution" / "mtx.h").write_text(HOST_MTX)
        (include / "coreinit" / "time.h").write_text(HOST_TIME)
        (include / "whb" / "log.h").write_text(HOST_LOG)
        (include / "whb" / "proc.h").write_text(HOST_PROC)
        harness = temporary_path / "entry_host.cpp"
        harness.write_text(HOST_HARNESS)
        executable = temporary_path / "entry_host"
        compile_result = subprocess.run(
            [
                "g++", "-std=c++17", "-O0", "-ffunction-sections", "-fdata-sections",
                "-Wno-unused-function", "-I", str(include),
                "-I", str(runtime.parent), "-I", str(ROOT / "games/smg/include"),
                "-I", str(ROOT / "platform"), str(runtime), str(harness),
                "-Wl,--gc-sections", "-o", str(executable),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            return False, "host harness compile failed:\n" + compile_result.stdout + compile_result.stderr
        run_result = subprocess.run(
            [str(executable), str(temporary_path / "card")],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        output = run_result.stdout + run_result.stderr
        if run_result.returncode:
            return False, f"host harness failed ({run_result.returncode}):\n{output.rstrip()}"
        return True, output.rstrip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=int, required=True)
    parser.add_argument("--require-run-game", type=int, choices=(0, 1))
    parser.add_argument("--require-single-entry", action="store_true")
    parser.add_argument("--require-single-procui-owner", action="store_true")
    parser.add_argument("--require-stage2-log", action="store_true")
    args = parser.parse_args()
    if args.stage != 2:
        return fail("only Stage 2 is supported")

    root_cmake = (ROOT / "CMakeLists.txt").read_text()
    main_source = (ROOT / "games/smg/platform/wiiu/main.cpp").read_text()
    runtime_source = (ROOT / "games/smg/platform/wiiu/runtime.cpp").read_text()
    platform_cmake = (ROOT / "platform/wiiu/CMakeLists.txt").read_text()
    vi_source = (ROOT / "platform/wiiu/vi.cpp").read_text()

    run_game_values = [line for line in root_cmake.splitlines() if "SMG_WIIU_RUN_GAME=" in line]
    if args.require_run_game is not None and run_game_values != [
        "      NDEBUG=1 VERSION=0 PORT_GAME_SMG=1 SMG_WIIU_RUN_GAME=1)"
    ]:
        return fail("SMG_WIIU_RUN_GAME=1 is not the unconditional SMG value")

    entry_calls = len(__import__("re").findall(r"\bs_game_entry\s*\(\s*\)", main_source))
    if args.require_single_entry and entry_calls != 1:
        return fail(f"expected one port_game_main entry call, found {entry_calls}")

    procui_owners = len(__import__("re").findall(r"\bWHBProcIsRunning\s*\(", vi_source))
    if args.require_single_procui_owner:
        if procui_owners != 1:
            return fail(f"expected one ProcUI pump owner, found {procui_owners}")
        if "WHBProcIsRunning" in main_source:
            return fail("SMG main retains a second ProcUI owner")
        if main_source.count("WHBProcInit(") != 1 or main_source.count("WHBProcShutdown(") != 1:
            return fail("SMG main does not have one ProcUI lifetime")

    if args.require_stage2_log:
        required_fragments = (
            '"%sstage2log"', '"%sstage2.log"', 'std::fopen(marker_path, "rb")',
            'std::fopen(log_path, "ab")', "std::fflush(log)", "std::fclose(log)",
        )
        missing = [fragment for fragment in required_fragments if fragment not in runtime_source]
        if missing:
            return fail("stage2.log safety boundary missing: " + ", ".join(missing))
        if "FILE* s_log" in runtime_source or "FILE* s_stage2" in runtime_source:
            return fail("stage2.log has a persistent FILE*")
        ok, output = check_host_logging(ROOT / "games/smg/platform/wiiu/runtime.cpp")
        if not ok:
            return fail(output)

    if "port_noaudio_init" in main_source or "port_init(" in main_source:
        return fail("SMG entry calls a forbidden shared runtime/audio owner")
    if "SMG_WIIU_RUN_GAME" in main_source:
        return fail("SMG main retains the Stage 1 run-game branch")
    if "[smg-audio] placeholder until Task 10" not in runtime_source:
        return fail("Task 10 audio boundary placeholder is missing")
    if runtime_source.count("platform/wiiu/runtime.cpp"):
        return fail("SMG runtime accidentally references shared runtime source")
    smg_platform_block = platform_cmake.split(
        'if(PORT_GAME STREQUAL "smg")', 1
    )[1].split("return()", 1)[0]
    if smg_platform_block.count("platform/audio/noaudio.cpp") != 1:
        return fail("SMG provider exclusion noaudio.cpp changed unexpectedly")
    if "OSReport" in main_source or "OSReport" in runtime_source:
        return fail("SMG entry uses OSReport")

    print("entry-boundary: PASS: run_game=1 entry_calls=1 procui_owners=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
