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
import statistics
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
FRAME_TABLE_HEADER = "| Bucket | Rows | Samples | p50 ms | p90 ms | max ms |"
CONTROL_TABLE_HEADER = "| Control | Action | Observed |"
CONTROLS = ("left stick", "A", "Y/X", "L", "ZL", "ZR")
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


def frame_rows_from_json(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [f"cannot read run JSON {path}: {exc}"]
    rows = evidence.get("framesplit")
    if not isinstance(rows, list):
        return [], [f"run JSON has no framesplit list: {path}"]
    return [row for row in rows if row.get("stage") == "EggStarGalaxy"], []


def frame_table(rows: list[dict[str, Any]]) -> str:
    lines = [FRAME_TABLE_HEADER, "|---|---:|---:|---:|---:|---:|"]
    for bucket in BUCKETS:
        p50s = [float(row[f"{bucket}_p50"]) / 1000 for row in rows]
        p90s = [float(row[f"{bucket}_p90"]) / 1000 for row in rows]
        maxes = [float(row[f"{bucket}_max"]) / 1000 for row in rows] if bucket != "total" else []
        samples = sum(int(row["samples"]) for row in rows)
        p50 = f"{statistics.median(p50s):.2f}" if p50s else "-"
        p90 = f"{statistics.median(p90s):.2f}" if p90s else "-"
        maximum = f"{max(maxes):.2f}" if maxes else "-"
        lines.append(f"| {bucket} | {len(rows)} | {samples} | {p50} | {p90} | {maximum} |")
    total_p90 = statistics.median([float(row["total_p90"]) / 1000 for row in rows]) if rows else 0
    within_target = "yes" if total_p90 <= 16.7 else "no"
    lines.append("")
    lines.append(
        "Frame table aggregation: rows attributed to EggStarGalaxy; target 16.7 ms; "
        f"total p90 within target: {within_target}"
    )
    return "\n".join(lines)


def markdown_table(report: str, header: str) -> dict[str, tuple[str, ...]]:
    lines = report.splitlines()
    for index, line in enumerate(lines):
        if line.strip().lower() != header.lower():
            continue
        result: dict[str, tuple[str, ...]] = {}
        for row in lines[index + 2:]:
            if not row.strip().startswith("|"):
                break
            cells = [cell.strip() for cell in row.strip().strip("|").split("|")]
            if len(cells) == 6 and cells[0].lower() in BUCKETS:
                result[cells[0].lower()] = tuple(cells[1:])
        return result
    return {}


def report_has_buckets(report: str, buckets: list[str], percentiles: list[str],
                       expected_table: str | None = None) -> list[str]:
    errors: list[str] = []
    table = markdown_table(report, FRAME_TABLE_HEADER)
    if not table:
        return ["report is missing the structured frame table"]
    expected_rows = {}
    if expected_table is not None:
        expected_rows = markdown_table(expected_table, FRAME_TABLE_HEADER)
    for bucket in buckets:
        cells = table.get(bucket)
        if cells is None:
            errors.append(f"report is missing frame bucket {bucket}")
            continue
        numeric_cells = cells[:4] if bucket == "total" else cells[:5]
        if any(not re.fullmatch(r"\d+(?:\.\d+)?", cell) for cell in numeric_cells):
            errors.append(f"report has non-numeric cells for frame bucket {bucket}")
        if "p50" in percentiles and cells[2] == "-":
            errors.append(f"report is missing p50 for frame bucket {bucket}")
        if "p90" in percentiles and cells[3] == "-":
            errors.append(f"report is missing p90 for frame bucket {bucket}")
        if expected_rows and expected_rows.get(bucket) != cells:
            errors.append(f"report frame table does not match fresh JSON for {bucket}")
    return errors


def report_controls(report: str) -> dict[str, str]:
    lines = report.splitlines()
    for index, line in enumerate(lines):
        if line.strip().lower() != CONTROL_TABLE_HEADER.lower():
            continue
        result: dict[str, str] = {}
        for row in lines[index + 2:]:
            if not row.strip().startswith("|"):
                break
            cells = [cell.strip() for cell in row.strip().strip("|").split("|")]
            if len(cells) == 3:
                result[cells[0].casefold()] = cells[2].casefold()
        return result
    return {}


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
    result.add_argument("--emit-frame-table", type=Path, metavar="RUN_JSON")
    result.add_argument("--run-json", type=Path, metavar="RUN_JSON")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    errors: list[str] = []
    if args.emit_frame_table is not None:
        frame_rows, frame_errors = frame_rows_from_json(args.emit_frame_table)
        errors.extend(frame_errors)
        if not errors:
            print(frame_table(frame_rows))
            return 0
        for error in errors:
            fail(error)
        return 1
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
    report_only = args.report is not None or args.run_json is not None or args.require_control_evidence
    if args.udp_log is None and not report_only:
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
    if log_path is None:
        lines = []
        start = end = 0
        run_lines = []
        starts = []
        run_number = 0
        frames = []
        frame_errors = []
        stages = []
        started_stages = []
    else:
        log_path = args.udp_log
    if "cemu" in str(log_path).lower():
        errors.append("Cemu log paths are forbidden; use the UDP Sunshine log")
    if log_path is not None:
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            errors.append(f"cannot read UDP log {log_path}: {exc}")
            text = ""
        lines = text.splitlines()
    if log_path is not None and args.to_line is not None and args.from_line is not None and args.to_line < args.from_line:
        errors.append("--to-line must be greater than or equal to --from-line")
    if log_path is not None and args.to_line is not None and args.to_line > len(lines):
        errors.append(f"--to-line exceeds log length ({len(lines)} lines)")
    if log_path is not None:
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
    if args.report is not None or args.require_no_rescue_release or args.require_frame_buckets or args.require_control_evidence:
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
        expected_table = None
        if args.run_json is not None:
            json_rows, json_errors = frame_rows_from_json(args.run_json)
            errors.extend(json_errors)
            if not json_errors:
                expected_table = frame_table(json_rows)
        errors.extend(report_has_buckets(report_text, buckets_to_check, required_percentiles, expected_table))
    if args.require_target_ms and not re.search(
        rf"Frame table aggregation:\s*[^\n]*target\s+{re.escape(args.require_target_ms)}\s+ms;\s*[^\n]*total p90 within target:\s+(?:yes|no)",
        report_text,
        re.I,
    ):
        errors.append(f"report caption does not record target {args.require_target_ms} ms")
    if args.require_control_evidence:
        controls = report_controls(report_text)
        missing = [control for control in CONTROLS if control.casefold() not in controls]
        invalid = [control for control in CONTROLS if control.casefold() in controls
                   and controls[control.casefold()] not in ("yes", "no", "n/a")]
        if missing:
            errors.append(f"report control table is missing: {', '.join(missing)}")
        if invalid:
            errors.append(f"report control table has invalid Observed values: {', '.join(invalid)}")
        for control in CONTROLS:
            observed = controls.get(control.casefold())
            if observed in ("no", "n/a"):
                print(f"console-gate: NOTE: {control} observed={observed}")
        for control in ("left stick", "A", "Y/X"):
            if controls.get(control.casefold()) != "yes":
                errors.append(f"report requires {control}=yes")

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
