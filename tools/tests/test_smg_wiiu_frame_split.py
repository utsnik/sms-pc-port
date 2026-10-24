#!/usr/bin/env python3
"""Check the SMG one-second frame-time split source contract."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def fail(message: str) -> str:
    return f"frame-split: FAIL: {message}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profiler", type=Path, required=True)
    parser.add_argument("--vi", type=Path, required=True)
    parser.add_argument("--gx", type=Path, nargs="+", required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--require-bucket", action="append", default=[])
    parser.add_argument("--require-percentiles")
    parser.add_argument("--require-udp-window", type=int)
    parser.add_argument("--require-stage-path")
    parser.add_argument("--require-target-ms")
    args = parser.parse_args()

    paths = [args.profiler, args.vi, *args.gx, args.audio]
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        print(fail("missing input: " + ", ".join(missing)))
        return 2

    profiler = args.profiler.read_text(encoding="utf-8")
    vi = args.vi.read_text(encoding="utf-8")
    gx = "\n".join(path.read_text(encoding="utf-8") for path in args.gx)
    audio = args.audio.read_text(encoding="utf-8")
    all_source = "\n".join((profiler, vi, gx, audio))
    errors: list[str] = []

    hooks = (
        "sms_wiiu_frame_split_begin",
        "sms_wiiu_frame_split_end",
        "sms_wiiu_frame_split_gx_begin",
        "sms_wiiu_frame_split_gx_end",
        "sms_wiiu_frame_split_gpu_begin",
        "sms_wiiu_frame_split_gpu_end",
        "sms_wiiu_frame_split_audio_begin",
        "sms_wiiu_frame_split_audio_end",
    )
    for hook in hooks:
        if not re.search(rf"\b{re.escape(hook)}\s*\(", profiler if hook.startswith("sms_wiiu_frame_split_") else all_source):
            errors.append(f"missing hook {hook}")

    for bucket in args.require_bucket:
        if bucket not in profiler:
            errors.append(f"missing bucket {bucket}")

    if args.require_percentiles:
        for percentile in args.require_percentiles.split(","):
            if percentile and percentile not in profiler:
                errors.append(f"missing percentile {percentile}")
    if args.require_udp_window is not None:
        window = str(args.require_udp_window)
        if window not in profiler or "port_log" not in profiler:
            errors.append(f"missing {window}-field port_log window")
    if args.require_stage_path and "sms_wiiu_stage_path()" not in profiler:
        errors.append("stage path is not included in the report")
    if args.require_target_ms and f"target-ms={args.require_target_ms}" not in profiler:
        errors.append("target frame time is not included in the report")

    if "audio-overlap=thread" not in profiler:
        errors.append("audio overlap qualifier is missing")
    if "framesplit" not in profiler:
        errors.append("framesplit marker is missing")
    if not re.search(r"kFrameSplitWindow\s*=\s*60", profiler):
        errors.append("frame split window is not fixed at 60 fields")
    if "std::atomic" in profiler or "thread_local" in all_source:
        errors.append("forbidden TLS or atomic timing state")
    gpu_window = re.search(
        r"sms_wiiu_frame_split_gpu_begin\s*\(\)([\s\S]*?)"
        r"sms_wiiu_frame_split_gpu_end\s*\(\)", gx)
    if gpu_window and "GX2DrawDone" in gpu_window.group(1):
        errors.append("frame split must not add GX2DrawDone measurement fences")

    if not re.search(r"sms_wiiu_frame_split_begin\s*\(\)[\s\S]*sms_wiiu_frame_split_end\s*\(", vi):
        errors.append("VI does not bracket the main frame")
    if not re.search(r"sms_wiiu_frame_split_gpu_begin\s*\(\)[\s\S]*sms_wiiu_gx2_target_present[\s\S]*sms_wiiu_frame_split_gpu_end\s*\(", gx):
        errors.append("GPU wait does not bracket target present")
    if not re.search(r"sms_wiiu_frame_split_audio_begin\s*\(\)[\s\S]*dma_block\s*\(\)[\s\S]*sms_wiiu_frame_split_audio_end\s*\(", audio):
        errors.append("audio bucket does not bracket dma_block")

    # The expensive renderer timestamps and the 30-frame report must be
    # completely marker-gated in the Galaxy build.  The implementation uses
    # g_perfStages, which is set only by the perfdiag marker.
    if not re.search(r"#if defined\(PORT_GAME_SMG\)[\s\S]*?g_perfStages[\s\S]*?#else", gx):
        errors.append("SMG perfdiag gating is missing from renderer timing code")
    if "PerfScope perfFlush" not in gx:
        errors.append("flushBatch PerfScope is missing")
    if not re.search(r"if\s*\(g_perfStages\)\s*\{[\s\S]*?\[perf\]", gx):
        errors.append("30-frame renderer report is not gated by perfdiag")
    if not re.search(r"g_perfStages\s*\?\s*OSGetTime\(\)\s*:\s*0", gx):
        errors.append("SMG renderer timestamps are not zero when perfdiag is absent")

    if errors:
        for error in errors:
            print(fail(error))
        return 1

    print("frame-split: PASS: buckets=logic_cpu,gx_translate,gpu_wait,audio window=60")
    print("frame-split: PASS: percentiles=p50,p90 target-ms=16.7 UDP=port_log")
    print("frame-split: PASS: audio is reported as overlapping audio-thread CPU time")
    return 0


if __name__ == "__main__":
    sys.exit(main())
