#!/usr/bin/env python3
"""Exercise the real SMG Wii U NAND provider at the host boundary."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> int:
    print(f"nand: FAIL: {message}")
    return 1


HOST_NAND_HEADER = r'''
#pragma once

typedef signed int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int BOOL;
typedef s32 ISFSError;

#define TRUE 1
#define FALSE 0
#define NAND_RESULT_OK 0
#define NAND_RESULT_ACCESS (-1)
#define NAND_RESULT_EXISTS (-6)
#define NAND_RESULT_INVALID (-8)
#define NAND_RESULT_MAXBLOCKS (-9)
#define NAND_RESULT_MAXFD (-10)
#define NAND_RESULT_NOEXISTS (-12)
#define NAND_RESULT_NOTEMPTY (-13)
#define NAND_RESULT_UNKNOWN (-64)
#define NAND_MAX_PATH 64
#define NAND_BANNER_SIGNATURE 0x5749424E
#define NAND_BANNER_COMMENT_SIZE 32
#define NAND_BANNER_TEXTURE_SIZE (192 * 64 * 2)
#define NAND_BANNER_ICON_SIZE (48 * 48 * 2)

typedef enum { NAND_ACCESS_NONE, NAND_ACCESS_READ, NAND_ACCESS_WRITE, NAND_ACCESS_RW } NANDAccessType;
typedef enum {
    NAND_PERM_RUSR = (NAND_ACCESS_READ << 4), NAND_PERM_WUSR = (NAND_ACCESS_WRITE << 4),
    NAND_PERM_RGRP = (NAND_ACCESS_READ << 2), NAND_PERM_WGRP = (NAND_ACCESS_WRITE << 2),
    NAND_PERM_ROTH = (NAND_ACCESS_READ << 0), NAND_PERM_WOTH = (NAND_ACCESS_WRITE << 0),
    NAND_PERM_RALL = NAND_PERM_RUSR | NAND_PERM_RGRP | NAND_PERM_ROTH,
    NAND_PERM_WALL = NAND_PERM_WUSR | NAND_PERM_WGRP | NAND_PERM_WOTH,
    NAND_PERM_RWALL = NAND_PERM_RALL | NAND_PERM_WALL
} NANDPermission;
typedef struct NANDFileInfo {
    s32 fileDescriptor; s32 origFd; char origPath[NAND_MAX_PATH];
    char tmpPath[NAND_MAX_PATH]; u8 accType; u8 stage; u8 mark;
} NANDFileInfo;
// Keep NANDCommandBlock opaque: the host revolution/nand.h shim intentionally
// provides only enough shape for callback pointers.
typedef struct NANDCommandBlock { void* unused; } NANDCommandBlock;
typedef struct NANDStatus { u32 ownerId; u16 groupId; u8 attribute; u8 permission; } NANDStatus;
typedef void (*NANDCallback)(s32, NANDCommandBlock*);
typedef struct {
    u32 signature; u32 flag; u16 iconSpeed; u8 reserved[22];
    u16 comment[2][NAND_BANNER_COMMENT_SIZE];
    u8 bannerTexture[NAND_BANNER_TEXTURE_SIZE];
    u8 iconTexture[8][NAND_BANNER_ICON_SIZE];
} NANDBanner;

#ifdef __cplusplus
extern "C" {
#endif
s32 NANDInit(void);
s32 NANDGetHomeDir(char[NAND_MAX_PATH]);
s32 NANDCreate(const char*, u8, u8);
s32 NANDPrivateCreate(const char*, u8, u8);
s32 NANDOpen(const char*, NANDFileInfo*, u8);
s32 NANDPrivateOpen(const char*, NANDFileInfo*, u8);
s32 NANDOpenAsync(const char*, NANDFileInfo*, u8, NANDCallback, NANDCommandBlock*);
s32 NANDClose(NANDFileInfo*);
s32 NANDCloseAsync(NANDFileInfo*, NANDCallback, NANDCommandBlock*);
s32 NANDRead(NANDFileInfo*, void*, u32);
s32 NANDWrite(NANDFileInfo*, const void*, u32);
s32 NANDGetLength(NANDFileInfo*, u32*);
s32 NANDDelete(const char*);
s32 NANDMove(const char*, const char*);
s32 NANDGetStatus(const char*, NANDStatus*);
s32 NANDPrivateGetStatus(const char*, NANDStatus*);
s32 NANDCheck(u32, u32, u32*);
s32 NANDPrivateDeleteAsync(const char*, NANDCallback, NANDCommandBlock*);
s32 NANDPrivateCreateAsync(const char*, u8, u8, NANDCallback, NANDCommandBlock*);
s32 nandIsInitialized(void);
void NANDInitBanner(NANDBanner*, u32, const u16*, const u16*);
#ifdef __cplusplus
}
#endif
'''


HARNESS = r'''
#include <revolution/nand.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static std::string g_root;

extern "C" const char* smg_wiiu_save_root(void) { return g_root.c_str(); }

static void fail(const char* message) {
    std::fprintf(stderr, "nand-host: FAIL: %s\n", message);
    std::exit(1);
}

static void check(bool condition, const char* message) {
    if (!condition) fail(message);
}

static void callback(s32 result, NANDCommandBlock*) {
    if (result != NAND_RESULT_OK) fail("async callback result");
}

int main(int argc, char** argv) {
    if (argc != 3) fail("usage");
    g_root = argv[1];
    const char* home = argv[2];
    const char* save_name = "GameData.bin";
    const u32 file_size = 0x10000;

    check(NANDInit() == NAND_RESULT_OK, "NANDInit");
    char actual_home[NAND_MAX_PATH] = {};
    check(NANDGetHomeDir(actual_home) == NAND_RESULT_OK, "NANDGetHomeDir");
    check(std::strcmp(actual_home, home) == 0, "NANDGetHomeDir value");

    NANDFileInfo info = {};
    check(NANDOpen(save_name, &info, NAND_ACCESS_READ) == NAND_RESULT_NOEXISTS,
          "missing GameData.bin must be NOEXISTS");
    u32 answer = 99;
    check(NANDCheck(5, 2, &answer) == NAND_RESULT_OK && answer == 0,
          "NANDCheck must report enough space");

    std::vector<unsigned char> written(file_size);
    for (u32 i = 0; i < file_size; ++i) written[i] = static_cast<unsigned char>(i * 37u);
    check(NANDCreate(save_name, NAND_PERM_RWALL, 0) == NAND_RESULT_OK, "NANDCreate");
    check(NANDOpen(save_name, &info, NAND_ACCESS_WRITE) == NAND_RESULT_OK, "write open");
    check(NANDWrite(&info, written.data(), file_size) == static_cast<s32>(file_size), "write");
    check(NANDClose(&info) == NAND_RESULT_OK, "write close");

    check(NANDOpen(save_name, &info, NAND_ACCESS_READ) == NAND_RESULT_OK,
          "read open must preserve existing save");
    u32 length = 0;
    check(NANDGetLength(&info, &length) == NAND_RESULT_OK && length == file_size,
          "NANDGetLength");
    std::vector<unsigned char> read(file_size);
    check(NANDRead(&info, read.data(), file_size) == static_cast<s32>(file_size), "read");
    check(read == written, "read-back");
    check(NANDClose(&info) == NAND_RESULT_OK, "read close");

    check(NANDCreate("/move-source.bin", NAND_PERM_RWALL, 0) == NAND_RESULT_OK,
          "NANDMove source create");
    std::filesystem::create_directories(
        std::filesystem::path(g_root) / "move-target");
    check(NANDMove("/move-source.bin", "/move-target") == NAND_RESULT_OK,
          "NANDMove inside save tree");
    check(NANDDelete("/move-target/move-source.bin") == NAND_RESULT_OK,
          "NANDDelete inside save tree");
    NANDStatus status = {};
    check(NANDGetStatus("/move-target", &status) == NAND_RESULT_OK,
          "NANDGetStatus inside save tree");
    check(NANDPrivateGetStatus("/move-target", &status) == NAND_RESULT_OK,
          "NANDPrivateGetStatus inside save tree");

    NANDCommandBlock block = {};
    check(NANDOpenAsync(save_name, &info, NAND_ACCESS_READ, callback, &block) == NAND_RESULT_OK,
          "async open");
    check(NANDCloseAsync(&info, callback, &block) == NAND_RESULT_OK, "async close");
    check(NANDPrivateCreateAsync("/async.bin", NAND_PERM_RWALL, 0, callback, &block) == NAND_RESULT_OK,
          "async create");
    check(NANDPrivateDeleteAsync("/async.bin", callback, &block) == NAND_RESULT_OK,
          "async delete");

    check(NANDOpen("/../escape.bin", &info, NAND_ACCESS_READ) == NAND_RESULT_INVALID,
          "absolute parent escape");
    check(NANDOpen("nested/../../escape.bin", &info, NAND_ACCESS_READ) == NAND_RESULT_INVALID,
          "relative parent escape");
    check(NANDCreate("/../escape.bin", NAND_PERM_RWALL, 0) == NAND_RESULT_INVALID,
          "create escape");
    check(NANDDelete("/../escape.bin") == NAND_RESULT_INVALID, "delete escape");
    check(NANDMove("/../escape.bin", "/") == NAND_RESULT_INVALID, "move escape");

    const std::filesystem::path home_root =
        std::filesystem::path(g_root) / "title/00010000/524d4750/data";
    const std::filesystem::path outside = std::filesystem::path(g_root).parent_path() / "outside";
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(home_root);
    std::filesystem::create_directory_symlink(outside, home_root / "link");
    check(NANDCreate("/title/00010000/524d4750/data/link/escape.bin",
                     NAND_PERM_RWALL, 0) == NAND_RESULT_INVALID,
          "symlink escape must be rejected");

    std::puts("nand-host: PASS: real nand.cpp clean create/write/read; escapes rejected");
    return 0;
}
'''


def provider_symbols(source: str) -> tuple[list[str], list[str]]:
    names = (
        "NANDInit", "NANDGetHomeDir", "NANDCreate", "NANDPrivateCreate",
        "NANDOpen", "NANDPrivateOpen", "NANDOpenAsync", "NANDPrivateOpenAsync",
        "NANDClose", "NANDCloseAsync", "NANDRead", "NANDReadAsync",
        "NANDWrite", "NANDWriteAsync", "NANDSeek", "NANDSeekAsync",
        "NANDGetLength", "NANDDelete", "NANDPrivateDelete", "NANDMove",
        "NANDCheck", "NANDGetStatus", "NANDPrivateGetStatus",
        "NANDPrivateGetStatusAsync", "NANDPrivateDeleteAsync",
        "NANDPrivateGetTypeAsync", "NANDPrivateCreateDirAsync",
        "NANDPrivateCreateAsync", "NANDInitBanner",
    )
    missing = [name for name in names if f"{name}(" not in source]
    return list(names), missing


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--virtual-home", required=True)
    parser.add_argument("--clean-create", action="store_true")
    args = parser.parse_args()

    if not args.source.is_file():
        return fail("unresolved NAND owner: provider source is missing")
    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(f"cannot read provider: {exc}")
    _, missing = provider_symbols(source)
    if missing:
        return fail("unresolved NAND owner: missing " + ", ".join(missing))
    if "smg_wiiu_save_root()" not in source:
        return fail("unsafe path mapping: provider has no runtime save-root boundary")
    names, _ = provider_symbols(source)
    stubs = args.source.parent / "sdk_stubs.h"
    try:
        stub_source = stubs.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(f"cannot read stub manifest: {exc}")
    remaining = [name for name in names if re.search(rf"\bX\({name}\)\b", stub_source)]
    if remaining:
        return fail("NAND symbols remain in Stage 1 stubs: " + ", ".join(remaining))
    cmake = args.source.parents[4] / "platform" / "wiiu" / "CMakeLists.txt"
    try:
        cmake_source = cmake.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(f"cannot read provider manifest: {exc}")
    if len(re.findall(r"platform/wiiu/nand\.cpp", cmake_source)) != 1:
        return fail("NAND provider is not selected exactly once")

    with tempfile.TemporaryDirectory(prefix="smg-nand-") as temporary:
        root = args.root or Path(temporary) / "save"
        root.mkdir(parents=True, exist_ok=True)
        if args.clean_create:
            for child in root.iterdir():
                if child.is_dir():
                    import shutil
                    shutil.rmtree(child)
                else:
                    child.unlink()
        harness = Path(temporary) / "nand_host.cpp"
        harness.write_text(HARNESS, encoding="utf-8")
        host_include = Path(temporary) / "include" / "revolution"
        host_include.mkdir(parents=True)
        (host_include / "nand.h").write_text(HOST_NAND_HEADER, encoding="utf-8")
        executable = Path(temporary) / "nand_host"
        result = subprocess.run(
            [
                "g++", "-std=c++17", "-Wall", "-Wextra",
                "-fpermissive", "-Wno-unused-function", "-Wno-ignored-qualifiers",
                "-I", str(Path(temporary) / "include"),
                "-I", str(args.source.parent),
                str(args.source), str(harness), "-o", str(executable),
            ], capture_output=True, text=True,
        )
        if result.returncode:
            return fail("host harness compile failed:\n" + result.stdout + result.stderr)
        result = subprocess.run(
            [str(executable), str(root), args.virtual_home],
            capture_output=True, text=True,
        )
        output = result.stdout + result.stderr
        if result.returncode:
            return fail(f"host harness failed ({result.returncode}):\n{output.rstrip()}")
        print(output.rstrip())
    print("nand: PASS: clean create/write/read; root escapes rejected")
    print("nand: PASS: one provider for each NAND symbol")
    return 0


if __name__ == "__main__":
    sys.exit(main())
