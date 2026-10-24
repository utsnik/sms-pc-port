#!/usr/bin/env python3
"""Offline evidence gate for the SMG Wii U Stage 3 console run.

The gate only reads its arguments.  In particular, it never opens a network
socket and never changes the UDP log.  By default a run is the suffix
beginning at the last ``SMG Wii U: main entered`` marker.  ``--from-line``
selects the append-only log window after a recorded pre-launch line count.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


MAIN_MARKER = "SMG Wii U: main entered"
SCENE_RE = re.compile(
    r"\[smg-scene\]\s+(?P<event>init|start)\s+"
    r"scene=(?P<scene>\S+)\s+stage=(?P<stage>\S*)"
)
TIMESTAMP_RE = re.compile(
    r"^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]|"
    r"^(?:\[)?(\d{4}-\d{2}-\d{2}(?:T| )\d{2}:[0-9:.+-]+Z?)(?:\])?\s*"
)
FRAME_RE = re.compile(
    r"^\[framesplit\]\s+frame=(?P<frame>\d+)\s+stage=(?P<stage>\S+)\s+"
    r"samples=(?P<samples>\d+)\s+fields=(?P<fields>\d+)\s+"
    r"logic_cpu\[p50=(?P<logic_cpu_p50>[0-9.]+)\s+p90=(?P<logic_cpu_p90>[0-9.]+)\s+"
    r"max=(?P<logic_cpu_max>[0-9.]+)\]\s+"
    r"\(logic_cpu=residual-incl-gx-writers\)\s+"
    r"gx_translate\[p50=(?P<gx_translate_p50>[0-9.]+)\s+p90=(?P<gx_translate_p90>[0-9.]+)\s+"
    r"max=(?P<gx_translate_max>[0-9.]+)\]\s+"
    r"gpu_wait\[p50=(?P<gpu_wait_p50>[0-9.]+)\s+p90=(?P<gpu_wait_p90>[0-9.]+)\s+"
    r"max=(?P<gpu_wait_max>[0-9.]+)\]\s+"
    r"audio\[p50=(?P<audio_p50>[0-9.]+)\s+p90=(?P<audio_p90>[0-9.]+)\s+"
    r"max=(?P<audio_max>[0-9.]+)\]\s+"
    r"total\[p50=(?P<total_p50>[0-9.]+)\s+p90=(?P<total_p90>[0-9.]+)\]\s+"
    r"target-ms=(?P<target_ms>[0-9.]+)\s+audio-overlap=thread\s*$"
)

BUCKETS = ("logic_cpu", "gx_translate", "gpu_wait", "audio", "total")
SOURCE_PATTERNS = {
    "main start": ("games/smg/platform/wiiu/main.cpp", "SMG Wii U: main entered"),
    "input diagnostics": ("games/smg/platform/wiiu/input.cpp", "[smg-input]"),
    "audio proof": ("platform/wiiu/audio.cpp", "[smg-audio] blocks=%u nonzero=%u"),
    "rescue thread started": ("platform/wiiu/exception.cpp", "[crash] rescue thread started"),
    "rescue takeover": ("platform/wiiu/exception.cpp", "[crash] rescue thread takes over"),
    "menu return": ("platform/wiiu/exception.cpp", "[crash] returning to the Wii U Menu"),
    "ProcUI exit": ("platform/wiiu/exception.cpp", "[crash] ProcUI exiting"),
    "frame split": ("platform/wiiu/profiler.cpp", "[framesplit] frame=%u stage=%s"),
    "stage heartbeat": ("platform/wiiu/vi.cpp", "SMS Wii U: frame=%u stage=%s"),
    "scene init": (
        "games/smg/decomp/src/Game/System/GameSystemSceneController.cpp",
        "[smg-scene] init scene=%s stage=%s",
    ),
    "scene start": (
        "games/smg/decomp/src/Game/System/GameSystemSceneController.cpp",
        "[smg-scene] start scene=%s stage=%s",
    ),
}


def fail(message: str) -> None:
    print(f"console-gate: FAIL: {message}")


def source_self_check(root: Path) -> list[str]:
    errors = []
    for name, (relative, pattern) in SOURCE_PATTERNS.items():
        path = root / relative
        try:
            source = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(f"{name}: cannot read {path}: {exc}")
            continue
        if pattern not in source:
            errors.append(f"{name}: source format string {pattern!r} is missing from {path}")
    return errors


def timestamp(line: str) -> str | None:
    match = TIMESTAMP_RE.match(line)
    return next((value for value in match.groups() if value is not None), None) if match else None


def read_report(path: Path) -> tuple[str, list[str]]:
    try:
        return path.read_text(encoding="utf-8", errors="replace"), []
    except OSError as exc:
        return "", [f"cannot read report {path}: {exc}"]


def scene_stage(line: str) -> tuple[str, str] | None:
    match = SCENE_RE.search(line)
    if not match:
        return None
    return match.group("event"), match.group("stage") or "unknown"


def parse_frames(lines: Iterable[tuple[int, str]]) -> tuple[list[dict[str, Any]], list[str]]:
    rows: list[dict[str, Any]] = []
    errors: list[str] = []
    initialized_stage = "unknown"
    started_stage: str | None = None
    for line_number, line in lines:
        event = scene_stage(line)
        if event is not None:
            kind, stage = event
            if kind == "start":
                started_stage = stage
            else:
                initialized_stage = stage
                started_stage = None
        if "[framesplit]" not in line:
            continue
        payload = line[line.index("[framesplit]"):].strip()
        match = FRAME_RE.match(payload)
        if not match:
            errors.append(f"malformed [framesplit] line {line_number}")
            continue
        row: dict[str, Any] = {"line": line_number}
        for key, value in match.groupdict().items():
            row[key] = value if key == "stage" else float(value) if "." in value else int(value)
        row["stage"] = started_stage if started_stage is not None else initialized_stage
        rows.append(row)
    return rows, errors


def scene_stages(lines: Iterable[str]) -> list[str]:
    stages: list[str] = []
    for line in lines:
        event = scene_stage(line)
        if event is None:
            continue
        _, stage = event
        if stage != "unknown" and stage not in stages:
            stages.append(stage)
    return stages


def scene_start_stages(lines: Iterable[str]) -> list[str]:
    stages: list[str] = []
    for line in lines:
        event = scene_stage(line)
        if event is None:
            continue
        kind, stage = event
        if kind == "start" and stage != "unknown" and stage not in stages:
            stages.append(stage)
    return stages


def report_has_buckets(report: str, buckets: list[str], percentiles: list[str]) -> list[str]:
    errors: list[str] = []
    lowered = report.lower()
    for bucket in buckets:
        if not re.search(rf"\b{re.escape(bucket)}\b", lowered):
            errors.append(f"report is missing frame bucket {bucket}")
        for percentile in percentiles:
            if not re.search(rf"\b{re.escape(bucket)}\b[^\n]*\b{re.escape(percentile)}\b", lowered):
                errors.append(f"report is missing {percentile} for frame bucket {bucket}")
        if bucket != "total" and not re.search(
            rf"\b{re.escape(bucket)}\b[^\n]*(?:count|samples)[^\n]*(?:p50|p90)[^\n]*max",
            lowered,
        ):
            errors.append(f"report is missing count/p50/p90/max row for {bucket}")
    return errors


def nonnegative_int(value: str) -> int:
    try:
        result = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a non-negative integer") from exc
    if result < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return result


def record_run(path: Path, log_path: Path, lines: list[str], start: int, end: int,
               frames: list[dict[str, Any]], stages: list[str], milestones: list[str],
               result: str, errors: list[str]) -> str | None:
    run_lines = lines[start:end]
    evidence: dict[str, Any] = {
        "result": result,
        "errors": errors,
        "udp_log": str(log_path),
        "run_interval": {
            "start_line": start + 1,
            "end_line": end,
            "main_start_count": sum(MAIN_MARKER in line for line in run_lines),
            "timestamps": {
                "start": timestamp(run_lines[0]) if run_lines else None,
                "end": timestamp(run_lines[-1]) if run_lines else None,
            },
        },
        "stage_path": stages,
        "framesplit": frames,
        "matched_milestones": milestones,
    }
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as exc:
        return f"cannot write run record {path}: {exc}"
    return None


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--udp-log", type=Path)
    result.add_argument("--source-root", type=Path, default=Path(__file__).parents[2])
    result.add_argument("--self-check", action="store_true")
    result.add_argument("--stage2-report", type=Path,
                        default=Path(__file__).parents[2] / "games/smg/platform/wiiu/STAGE2_REPORT.md")
    result.add_argument("--report", type=Path)
    result.add_argument("--record-run", type=Path)
    result.add_argument("--from-line", type=nonnegative_int, metavar="N",
                        help="use log lines after the N lines present before launch")
    result.add_argument("--to-line", type=nonnegative_int, metavar="M",
                        help="use no lines after inclusive 1-based log line M")
    result.add_argument("--require-log", action="append", default=[])
    result.add_argument("--require-stage-path")
    result.add_argument("--require-stage2-pass", action="store_true")
    result.add_argument("--require-main-start", action="store_true")
    result.add_argument("--require-no-notrap", action="store_true")
    result.add_argument("--require-single-launch", action="store_true")
    result.add_argument("--require-console-frame-split", action="store_true")
    result.add_argument("--require-good-egg", action="store_true")
    result.add_argument("--require-rescue-disabled-run", action="store_true")
    result.add_argument("--require-rescue-enabled-run", action="store_true")
    result.add_argument("--require-rescue-menu-return", action="store_true")
    result.add_argument("--require-frame-p50-p90", action="store_true")
    result.add_argument("--require-no-rescue-release", action="store_true")
    result.add_argument("--require-frame-buckets")
    result.add_argument("--require-frame-percentiles")
    result.add_argument("--require-target-ms")
    result.add_argument("--require-control-evidence", action="store_true")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    errors: list[str] = []
    if args.self_check:
        errors.extend(source_self_check(args.source_root))
        if not errors:
            print("console-gate: PASS: source log format self-check")
    needs_log = any(
        getattr(args, name)
        for name in (
            "require_stage2_pass", "require_main_start", "require_no_notrap",
            "require_single_launch", "require_console_frame_split", "require_good_egg",
            "require_rescue_disabled_run", "require_rescue_enabled_run",
            "require_rescue_menu_return", "require_frame_p50_p90",
        )
    ) or bool(args.require_log or args.require_stage_path or args.record_run)
    if needs_log and args.udp_log is None:
        errors.append("--udp-log is required for log checks")
    if args.udp_log is None:
        if errors:
            for error in errors:
                fail(error)
            return 1
        if args.self_check:
            return 0
        errors.append("no check was requested")
        fail(errors[-1])
        return 2

    log_path = args.udp_log
    if "cemu" in str(log_path).lower():
        errors.append("Cemu log paths are forbidden; use the UDP Sunshine log")
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        errors.append(f"cannot read UDP log {log_path}: {exc}")
        text = ""
    lines = text.splitlines()
    if args.to_line is not None and args.from_line is not None and args.to_line < args.from_line:
        errors.append("--to-line must be greater than or equal to --from-line")
    if args.to_line is not None and args.to_line > len(lines):
        errors.append(f"--to-line exceeds log length ({len(lines)} lines)")
    end = args.to_line if args.to_line is not None else len(lines)
    all_starts = [index for index, line in enumerate(lines) if MAIN_MARKER in line]
    if args.from_line is not None:
        start = args.from_line
        run_lines = lines[start:end]
        starts = [index for index in all_starts if start <= index < end]
    else:
        start = all_starts[-1] if all_starts else len(lines)
        run_lines = lines[start:end]
        starts = [index for index in all_starts if start <= index < end]
    run_number = len(starts)
    frames, frame_errors = parse_frames((index + 1, line) for index, line in enumerate(run_lines, start))
    errors.extend(frame_errors)
    stages = scene_stages(run_lines)
    started_stages = scene_start_stages(run_lines)

    if args.require_stage2_pass:
        report, report_errors = read_report(args.stage2_report)
        errors.extend(report_errors)
        if not re.search(r"cemu-gate:\s*PASS|Task 12 Cemu gate result[\s\S]{0,120}PASSED", report, re.I):
            errors.append(f"Stage 2 report has no cemu-gate PASS: {args.stage2_report}")
    if args.require_main_start and not starts:
        errors.append("missing SMG Wii U main-start marker")
    if args.require_single_launch:
        if args.from_line is None:
            errors.append("--require-single-launch requires --from-line")
        elif run_number != 1:
            errors.append(f"expected exactly one launch marker after --from-line; found {run_number}")
    if args.require_no_notrap and re.search(r"\b(?:cemu_)?notrap\b", "\n".join(run_lines), re.I):
        errors.append("notrap RPX name is present in the selected run")
    for pattern in args.require_log:
        try:
            matched = re.search(pattern, "\n".join(run_lines), re.I | re.M)
        except re.error as exc:
            matched = None
            errors.append(f"invalid --require-log regex {pattern!r}: {exc}")
        if not matched:
            errors.append(f"missing required run log pattern: /{pattern}/")
    if args.require_stage_path and not any(args.require_stage_path in stage for stage in stages):
        errors.append(f"required stage path {args.require_stage_path} was not observed in scene logs")
    good_egg_started = "EggStarGalaxy" in started_stages
    if args.require_good_egg and not good_egg_started:
        errors.append("Good Egg gate requires [smg-scene] start ... stage=EggStarGalaxy")
    if args.require_good_egg and (args.require_console_frame_split or args.require_frame_p50_p90) and not any(
        row["stage"] == "EggStarGalaxy" for row in frames
    ):
        errors.append("Good Egg frame evidence requires an EggStarGalaxy-attributed [framesplit] row")
    if args.require_console_frame_split and not frames:
        errors.append("missing console [framesplit] rows")
    if args.require_console_frame_split and frames and any(
        str(row["target_ms"]) != "16.7" for row in frames
    ):
        errors.append("frame split target-ms is not 16.7")
    if args.require_frame_p50_p90:
        if not frames:
            errors.append("missing frame p50/p90 rows")
        else:
            for row in frames:
                for bucket in BUCKETS:
                    if f"{bucket}_p50" not in row or f"{bucket}_p90" not in row:
                        errors.append(f"frame row {row['line']} is missing p50/p90 for {bucket}")
    evidence_text = "\n".join(run_lines) if args.from_line is not None else "\n".join(lines[:end])
    if args.require_rescue_disabled_run and not re.search(
        r"no[- ]rescue|rescue thread disabled.*SMG", evidence_text, re.I
    ):
        errors.append("no successful disabled-rescue run is identified in the UDP evidence")
    if args.require_rescue_enabled_run and not re.search(
        r"\[crash\] rescue thread started", "\n".join(run_lines), re.I
    ):
        errors.append("selected run has no rescue-thread-start evidence")
    if args.require_rescue_menu_return:
        selected = "\n".join(run_lines)
        for marker in ("[crash] returning to the Wii U Menu", "[crash] ProcUI exiting"):
            if marker not in selected:
                errors.append(f"selected rescue run is missing {marker}")

    report_text = ""
    if args.report is not None or args.require_no_rescue_release or args.require_frame_buckets:
        if args.report is None:
            errors.append("--report is required for report checks")
        else:
            report_text, report_errors = read_report(args.report)
            errors.extend(report_errors)
    if args.require_no_rescue_release:
        if not re.search(r"no[- ]rescue", report_text, re.I) or not re.search(
            r"release candidate|retained.*release", report_text, re.I
        ):
            errors.append("report does not identify the no-rescue release candidate")
    required_buckets = [item.strip() for item in (args.require_frame_buckets or "").split(",") if item.strip()]
    required_percentiles = [item.strip() for item in (args.require_frame_percentiles or "").split(",") if item.strip()]
    if required_buckets:
        unknown = [bucket for bucket in required_buckets if bucket not in BUCKETS]
        errors.extend(f"unknown frame bucket {bucket}" for bucket in unknown)
        buckets_to_check = [b for b in required_buckets if b in BUCKETS]
        if "total" not in buckets_to_check:
            buckets_to_check.append("total")
        errors.extend(report_has_buckets(report_text, buckets_to_check, required_percentiles))
    if args.require_target_ms and not re.search(
        rf"target-ms\s*[=:]?\s*{re.escape(args.require_target_ms)}|{re.escape(args.require_target_ms)}\s*ms",
        report_text,
        re.I,
    ):
        errors.append(f"report does not record target-ms={args.require_target_ms}")
    if args.require_control_evidence:
        controls = ("left stick", "A", "Y", "X", "L", "ZL", "ZR")
        if not all(re.search(rf"\b{re.escape(control)}\b", report_text, re.I) for control in controls):
            errors.append("report lacks evidence for the required basic controls")

    if args.record_run:
        milestones = list(args.require_log)
        if args.require_main_start:
            milestones.append(MAIN_MARKER)
        record_error = record_run(
            args.record_run, log_path, lines, start, end, frames, stages, milestones,
            "FAIL" if errors else "PASS", list(errors),
        )
        if record_error:
            errors.append(record_error)

    if errors:
        for error in errors:
            fail(error)
        return 1

    if args.require_no_notrap or args.require_single_launch:
        print("console-gate: PASS: one approved wiiload launch; no notrap RPX")
    if args.require_stage2_pass or args.require_log:
        print("console-gate: PASS: Stage 2 runtime/audio/input milestones observed on hardware")
    if args.require_good_egg:
        print("console-gate: PASS: stage=EggStarGalaxy (Good Egg) reached")
    if args.require_console_frame_split or args.require_frame_p50_p90:
        print("console-gate: PASS: frame split parsed for logic_cpu,gx_translate,gpu_wait,audio,total")
    if args.require_rescue_enabled_run:
        print("rescue-gate: PASS: rescue thread started only in opt-in test build")
    if args.require_rescue_menu_return:
        print("rescue-gate: PASS: crash dump reached UDP and Menu handoff completed")
    if args.require_no_rescue_release:
        print("console-gate: PASS: release is the no-rescue hardware variant")
    if args.require_control_evidence:
        print("console-gate: PASS: Good Egg Galaxy reached and basic controls exercised")
    if args.report is not None and args.require_frame_buckets:
        print("console-gate: PASS: frame split p50/p90 recorded for logic_cpu,gx_translate,gpu_wait,audio")
    if args.require_main_start and not re.search(r"\[crash\]", "\n".join(run_lines), re.I):
        print("console-gate: PASS: no crash; no FTP log read during launch")
    if args.require_control_evidence:
        print("console-gate: PASS: Stage 3 gate — start a file and play Good Egg Galaxy on console")
    if args.record_run:
        print(f"console-gate: recorded run evidence at {args.record_run}")
    if args.self_check and not needs_log:
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
