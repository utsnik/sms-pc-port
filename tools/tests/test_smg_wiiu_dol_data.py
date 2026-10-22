#!/usr/bin/env python3
"""Check that SMG loads the three native-BE objects from sys/main.dol."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> int:
    print(f"dol-data: FAIL: {message}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--expect-error", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    parser.add_argument("--expect-story", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    parser.add_argument("--expect-galaxy", type=lambda value: tuple(int(part, 0) for part in value.split(":")))
    args = parser.parse_args()

    try:
        source = args.source.read_text(encoding="utf-8")
    except OSError:
        return fail("missing main.dol loader source")
    source_dir = args.source.parent
    stubs = source_dir / "galaxy_link_stubs.cpp"
    bcsv_include = args.source.parents[2] / "decomp/src/Game/System/BCSVInclude.s"
    if not re.search(r"sms_wiiu_dvd_read_file\s*\(\s*\"/sys/main\.dol\"", source):
        return fail("missing main.dol loader through the Task 2 DVD boundary")
    if "native-BE" not in source or "alignas(32)" not in source:
        return fail("loader does not document native-BE aligned target storage")
    if "ErrorMessageArchive.arc" in source or "StoryEvent.bcsv" in source or "GalaxyID.bcsv" in source:
        return fail("loader still loads cooked sys/*.arc or sys/*.bcsv files")
    try:
        stubs_source = stubs.read_text(encoding="utf-8")
        bcsv_source = bcsv_include.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(str(exc))
    if re.search(r"(?:cErrorArchive|StoryEventBCSV|GalaxyIDBCSV)\s*\[[^]]*1[^]]*\]", stubs_source):
        return fail("Stage 1 one-byte table placeholder remains")
    if ".incbin" in bcsv_source:
        return fail("BCSVInclude.s still has a cooked-file .incbin provider")

    harness = Path(__file__).with_name("smg_dol_data_host.cpp")
    with tempfile.TemporaryDirectory(prefix="smg-dol-data-") as build_dir:
        executable = Path(build_dir) / "smg_dol_data_host"
        compile_result = subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(source_dir),
                "-I",
                str(args.source.parents[2] / "include"),
                str(args.source),
                str(harness),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            return fail(f"host harness compile failed:\n{compile_result.stdout}{compile_result.stderr}")
        run_result = subprocess.run(
            [str(executable), str(args.dol)],
            capture_output=True,
            text=True,
        )
        output = run_result.stdout + run_result.stderr
        if run_result.returncode:
            return fail(f"host harness failed ({run_result.returncode}):\n{output.rstrip()}")

    expected = (args.expect_error, args.expect_story, args.expect_galaxy)
    labels = ("ErrorMessageArchive", "StoryEvent", "GalaxyID")
    for label, wanted in zip(labels, expected):
        if wanted is not None:
            expected_text = f"{label} offset=0x{wanted[0]:x} size={wanted[1]}"
            if expected_text not in output:
                return fail(f"host harness output missing {expected_text}:\n{output.rstrip()}")
    print(f"dol-data: PASS: host harness\n{output.rstrip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
