#!/usr/bin/env python3
"""Check the SMG Wii U audio provider boundary and linked target."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys


REACHED = {
    "AIInit",
    "AICheckInit",
    "AIReset",
    "AIRegisterDMACallback",
    "AIInitDMA",
    "AIGetDMAEnableFlag",
    "AIStartDMA",
    "AIStopDMA",
    "AIGetDMABytesLeft",
    "DSPInit",
}


def fail(message: str) -> None:
    print(f"audio-boundary: FAIL: {message}")
    raise SystemExit(1)


def cmake_sources(cmake: str) -> tuple[str, str]:
    text = pathlib.Path(cmake).read_text()
    shared = re.search(
        r"set\(SMG_WIIU_SHARED_PROVIDER_SOURCES(?P<body>.*?)\n\s*\)",
        text,
        re.S,
    )
    platform = re.search(
        r"set\(SMG_WIIU_PLATFORM_SOURCES(?P<body>.*?)\n\s*\)",
        text,
        re.S,
    )
    if not shared or not platform:
        fail("SMG provider source manifests are missing")
    return shared.group("body"), platform.group("body")


def check_sources(args: argparse.Namespace) -> None:
    shared, platform = cmake_sources(args.cmake)
    selected = shared + platform
    def selected_in(body: str, path: str) -> bool:
        return path in body or path.rsplit("/", 1)[-1] in body and body.count(path.rsplit("/", 1)[-1]) > 0

    for source in ("platform/audio/dsp_hle.cpp", "platform/audio/dsp_mixer.cpp"):
        if not selected_in(shared, source):
            fail(f"{source} is not an SMG shared audio provider")
    for source in ("platform/wiiu/audio.cpp", "platform/wiiu/audio_sdl.cpp"):
        if not selected_in(platform, source):
            fail(f"{source} is not an SMG Wii U output provider")
    if selected_in(selected, "platform/ar/ar.cpp"):
        fail("SMG selects platform/ar/ar.cpp instead of the Galaxy aralt provider")
    for source in ("platform/audio/ai.cpp", "platform/audio/noaudio.cpp"):
        if selected_in(selected, source):
            fail(f"SMG selects forbidden silent/host provider {source}")

    cmake_path = pathlib.Path(args.cmake).resolve()
    root = cmake_path.parents[2]
    stubs = root / "games/smg/platform/wiiu/sdk_stubs.h"
    if not stubs.exists():
        fail(f"missing SMG stub manifest: {stubs}")
    stub_text = stubs.read_text()
    for symbol in REACHED | {"__DSP_boot_task"}:
        if re.search(rf"\b(?:smg_gc_)?{re.escape(symbol)}\b", stub_text):
            fail(f"reached symbol {symbol} remains in the SMG stub manifest")

    for source_path in args.sources:
        source = pathlib.Path(source_path).read_text()
        if source_path.endswith("audio.cpp") and "[smg-audio] blocks=" not in source:
            fail("SMG audio clock does not emit periodic proof-of-sound counters")
        if source_path.endswith("dsp_mixer.cpp") and "[smg-audio]" not in source:
            fail("SMG mixer does not emit active-voice diagnostics")
    overwrite = root / "games/smg/decomp/src/Game/System/Overwrite.cpp"
    overwrite_text = overwrite.read_text()
    if "#if defined(TARGET_WIIU)" not in overwrite_text or "port_mem1_base" not in overwrite_text:
        fail("JKRAramPiece::startDMA has no Wii U MEM1/MEM2 address guard")
    if args.require_source_manifest:
        if "SMS_AUDIO_DSP_HLE=1" not in pathlib.Path(args.cmake).read_text():
            fail("SMG source manifest does not enable SMS_AUDIO_DSP_HLE")
    print("audio-boundary: source providers: JAudio2, DSP HLE, and Wii U output selected")


def compile_db_entries(path: str) -> list[dict]:
    try:
        data = json.loads(pathlib.Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read compile database: {exc}")
    return data


def check_compile_db(args: argparse.Namespace) -> None:
    entries = compile_db_entries(args.compile_db)
    commands = [entry.get("command", "") + " " + " ".join(entry.get("arguments", []))
                if isinstance(entry.get("arguments"), list)
                else entry.get("command", "") for entry in entries]
    files = [entry.get("file", "") for entry in entries]
    joined = "\n".join(commands)
    for needle in ("platform/audio/dsp_hle.cpp", "platform/audio/dsp_mixer.cpp",
                   "platform/wiiu/audio.cpp", "platform/wiiu/audio_sdl.cpp"):
        if not any(needle in file for file in files):
            fail(f"compile database does not select {needle}")
    for forbidden in ("platform/audio/ai.cpp", "platform/audio/noaudio.cpp"):
        if any(forbidden in file for file in files):
            fail(f"compile database selects forbidden provider {forbidden}")
    if args.require_sms_audio_dsp_hle and "SMS_AUDIO_DSP_HLE=1" not in joined:
        fail("compile database is missing SMS_AUDIO_DSP_HLE=1")
    if args.require_noaudio_unselected and "platform/audio/noaudio.cpp" in joined:
        fail("compile database selects platform/audio/noaudio.cpp")
    if args.require_ai_unselected and "platform/audio/ai.cpp" in joined:
        fail("compile database selects platform/audio/ai.cpp")
    print("audio-boundary: compile database: providers and SMS_AUDIO_DSP_HLE selected")


def check_elf(elf: str) -> None:
    path = pathlib.Path(elf)
    if not path.is_file():
        fail(f"linked ELF not found: {elf}")
    checker = pathlib.Path("/home/utking/homelab/wiiu/tools/wiiu_port_check.py")
    if checker.is_file():
        result = subprocess.run([sys.executable, str(checker), "elf", str(path)],
                                text=True, capture_output=True)
        if result.returncode:
            fail("linked ELF audit failed: " + (result.stdout + result.stderr).strip())
    print("audio-boundary: linked ELF: Wii U ELF audit passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake")
    parser.add_argument("--sources", nargs="+", default=[])
    parser.add_argument("--elf")
    parser.add_argument("--compile-db")
    parser.add_argument("--require-source-manifest", action="store_true")
    parser.add_argument("--require-sms-audio-dsp-hle", action="store_true")
    parser.add_argument("--require-noaudio-unselected", action="store_true")
    parser.add_argument("--require-ai-unselected", action="store_true")
    args = parser.parse_args()
    if args.compile_db:
        check_compile_db(args)
    else:
        if not args.cmake:
            parser.error("--cmake is required when --compile-db is absent")
        check_sources(args)
    if args.elf:
        check_elf(args.elf)
    if args.compile_db:
        print("audio-boundary: PASS: JAudio2 -> DSP HLE -> Wii U AX/SDL")


if __name__ == "__main__":
    main()
