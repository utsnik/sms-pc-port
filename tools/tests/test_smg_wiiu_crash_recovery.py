#!/usr/bin/env python3
"""Check the SMG crash-recovery build boundary without running the console."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def fail(message: str) -> int:
    print(f"crash-boundary: FAIL: {message}")
    return 1


def function_body(source: str, signature: str) -> str | None:
    start = source.find(signature)
    if start < 0:
        return None
    opening = source.find("{", start)
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--main", type=Path, required=True)
    parser.add_argument("--exception", type=Path, required=True)
    parser.add_argument("--vi", type=Path, required=True)
    parser.add_argument("--require-smg-disabled-by-default", action="store_true")
    parser.add_argument("--require-no-unconditional-rescue", action="store_true")
    parser.add_argument("--require-install-after-vi-init", action="store_true")
    args = parser.parse_args()

    try:
        cmake = args.cmake.read_text()
        main_source = args.main.read_text()
        exception = args.exception.read_text()
        vi = args.vi.read_text()
    except OSError as error:
        return fail(str(error))

    if "exception.cpp" not in cmake:
        return fail("SMG does not compile the existing exception unit")

    rescue_option = re.search(
        r'option\(SMG_WIIU_ENABLE_RESCUE\b.*?\bOFF\)', cmake, re.DOTALL
    )
    if args.require_smg_disabled_by_default and rescue_option is None:
        return fail("SMG_WIIU_ENABLE_RESCUE is not disabled by default")
    if args.require_smg_disabled_by_default and re.search(
        r"SMG_WIIU_ENABLE_RESCUE\s*=\s*1", cmake
    ):
        return fail("SMG rescue is enabled unconditionally")

    if "SMG_WIIU_CRASH_TEST" not in cmake:
        return fail("SMG crash-test switch is not configured")
    if "SMG_WIIU_ENABLE_RESCUE=$<BOOL:${SMG_WIIU_ENABLE_RESCUE}>" not in cmake:
        return fail("SMG rescue option is not passed to the target")
    smg_compile_definitions = re.search(
        r"target_compile_definitions\(smg PRIVATE(.*?)\n\s*\)", cmake, re.DOTALL
    )
    if smg_compile_definitions is None:
        return fail("SMG executable target compile definitions are missing")
    if "SMG_WIIU_CRASH_TEST=$<BOOL:${SMG_WIIU_CRASH_TEST}>" not in smg_compile_definitions.group(1):
        return fail("SMG crash-test switch is not passed to the executable target")

    if "SMG Wii U: main entered" not in main_source:
        return fail("stage-neutral SMG main diagnostic is missing")
    if "stage2 main entered" in main_source or "stage3 main entered" in main_source:
        return fail("stage-specific SMG main diagnostic remains")
    if args.require_install_after_vi_init:
        vi_init = main_source.find("port_vi_init();")
        install = main_source.find("sms_wiiu_install_exception_handlers();")
        game = main_source.find("s_game_entry();")
        if vi_init < 0 or install < 0 or game < 0 or not vi_init < install < game:
            return fail("SMG handlers are not installed after VI owner setup")

    if "OSSetExceptionCallbackEx" not in exception:
        return fail("exception handlers are not installed")
    if "trace(context);" not in exception or "dump(context);" not in exception:
        return fail("fatal callback no longer emits stack/register records")
    if "OSFatal(s_message);" not in exception:
        return fail("no-rescue fatal path does not retain OSFatal")
    if "sms_wiiu_vi_owner_thread()" not in exception:
        return fail("rescue path lost the main-thread owner check")

    capture_guard = next(
        (
            match
            for match in re.finditer(
                r"#if defined\(PORT_GAME_SMG\) && SMG_WIIU_ENABLE_RESCUE(.*?)#endif",
                exception,
                re.DOTALL,
            )
            if "BOOL rescue_fatal(" in match.group(1)
        ),
        None,
    )
    if capture_guard is None:
        return fail("SMG rescue capture guard is missing")
    rescue_capture = function_body(capture_guard.group(1), "BOOL rescue_fatal(")
    if rescue_capture is None:
        return fail("SMG rescue callback is not isolated in rescue_fatal")
    forbidden = (
        "snprintf", "vsnprintf", "WHBLog", "OSGetSymbolName", "trace(",
        "dump(", "strlen(", "vsprintf", "vfprintf",
    )
    if any(token in rescue_capture for token in forbidden):
        return fail("SMG rescue callback performs formatting, symbol lookup, dumping, or logging")
    if "__sync_synchronize()" not in rescue_capture:
        return fail("SMG rescue callback does not publish through a memory barrier")
    if "faulting_thread" not in rescue_capture or "stack" not in rescue_capture:
        return fail("SMG rescue callback does not capture thread and stack state")
    if "if (!s_rescue_started)" not in capture_guard.group(1):
        return fail("SMG rescue callback does not fall back when the rescue thread is unavailable")
    if "could not start the rescue thread" not in exception:
        return fail("SMG rescue installation does not log thread-start failure")

    rescue_guard = next(
        (
            match
            for match in re.finditer(
                r"#if !defined\(PORT_GAME_SMG\) \|\| SMG_WIIU_ENABLE_RESCUE(.*?)#else",
                exception,
                re.DOTALL,
            )
            if "cafe_OSCreateThread(&s_rescue" in match.group(1)
        ),
        None,
    )
    if args.require_no_unconditional_rescue and (
        rescue_guard is None or "cafe_OSCreateThread(&s_rescue" not in rescue_guard.group(1)
    ):
        return fail("rescue thread creation is not gated by SMG_WIIU_ENABLE_RESCUE")

    crash_guard = re.search(
        r"#if defined\(PORT_GAME_SMG\)\n#if defined\(SMG_WIIU_CRASH_TEST\).*?#endif\n#else",
        vi,
        re.DOTALL,
    )
    if crash_guard is None:
        return fail("SMG crash trigger is not compile-time gated")
    if "kSmgCrashTestFrame = 300" not in crash_guard.group(0):
        return fail("SMG crash trigger is not fixed at the post-frame boundary")
    if "getenv" in crash_guard.group(0) or "SMS_CRASHTEST" in crash_guard.group(0):
        return fail("normal SMG builds retain an environment-triggered crash")
    if "SMS_CRASHTEST" not in vi:
        return fail("SMS crash-test branch was not preserved")

    print("crash-boundary: PASS: SMG handler installed after VI owner; rescue is opt-in")
    return 0


if __name__ == "__main__":
    sys.exit(main())
