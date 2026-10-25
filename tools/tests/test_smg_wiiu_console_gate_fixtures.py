#!/usr/bin/env python3
"""Fixture-driven unittest coverage for the offline console gate."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
GATE = HERE / "test_smg_wiiu_console_gate.py"
FIXTURES = HERE / "fixtures" / "smg_console_gate"


class ConsoleGateFixtures(unittest.TestCase):
    def run_gate(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(GATE), *arguments],
            cwd=HERE.parents[1],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_task5_pass_and_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "run.json"
            result = self.run_gate(
                "--udp-log", str(FIXTURES / "task5_pass.log"),
                "--stage2-report", str(FIXTURES / "stage2_pass_report.md"),
                "--from-line", "0",
                "--require-stage2-pass", "--require-main-start",
                "--require-no-notrap", "--require-single-launch",
                "--require-console-frame-split", "--require-good-egg",
                "--require-log", r"SMG Wii U: main entered",
                "--record-run", str(record),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("stage=EggStarGalaxy (Good Egg) reached", result.stdout)
            evidence = json.loads(record.read_text(encoding="utf-8"))
            self.assertEqual(evidence["result"], "PASS")
            self.assertEqual(evidence["errors"], [])
            self.assertEqual(evidence["run_interval"]["start_line"], 1)
            self.assertEqual(evidence["run_interval"]["main_start_count"], 1)
            self.assertEqual(len(evidence["framesplit"]), 2)
            self.assertEqual(
                [row["stage"] for row in evidence["framesplit"]],
                ["FileSelect", "EggStarGalaxy"],
            )
            self.assertEqual(evidence["stage_path"], ["FileSelect", "EggStarGalaxy"])

    def test_failed_run_is_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "run.json"
            result = self.run_gate(
                "--udp-log", str(FIXTURES / "heavens_only.log"),
                "--require-good-egg", "--record-run", str(record),
            )
            self.assertNotEqual(result.returncode, 0)
            evidence = json.loads(record.read_text(encoding="utf-8"))
            self.assertEqual(evidence["result"], "FAIL")
            self.assertTrue(evidence["errors"])

    def test_bounded_window_excludes_appended_rescue_run(self) -> None:
        fixture = FIXTURES / "no_rescue_then_rescue.log"
        passing = self.run_gate(
            "--udp-log", str(fixture), "--from-line", "0", "--to-line", "4",
            "--require-good-egg",
        )
        self.assertEqual(passing.returncode, 0, passing.stdout + passing.stderr)

        failing = self.run_gate(
            "--udp-log", str(fixture), "--require-good-egg",
        )
        self.assertNotEqual(failing.returncode, 0)
        self.assertIn("EggStarGalaxy", failing.stdout)

    def test_task6_rescue_and_report_pass(self) -> None:
        result = self.run_gate(
            "--udp-log", str(FIXTURES / "rescue_pass.log"),
            "--report", str(FIXTURES / "fixture_report.md"),
            "--require-rescue-disabled-run", "--require-rescue-enabled-run",
            "--require-rescue-menu-return", "--require-frame-p50-p90",
            "--require-good-egg", "--require-no-rescue-release",
            "--require-frame-buckets", "logic_cpu,gx_translate,gpu_wait,audio",
            "--require-frame-percentiles", "p50,p90", "--require-target-ms", "16.7",
            "--require-stage-path", "EggStarGalaxy", "--require-control-evidence",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Stage 3 gate", result.stdout)

    def test_negative_fixtures(self) -> None:
        cases = (
            (("notrap.log", "--require-main-start", "--require-no-notrap"), "notrap RPX"),
            (("two_launch.log", "--from-line", "0", "--require-main-start", "--require-single-launch"), "exactly one"),
            (("heavens_only.log", "--require-main-start", "--require-good-egg"), "EggStarGalaxy"),
        )
        for (fixture, *arguments), expected in cases:
            with self.subTest(fixture=fixture):
                result = self.run_gate("--udp-log", str(FIXTURES / fixture), *arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout)

        result = self.run_gate(
            "--udp-log", str(FIXTURES / "cemu_run.log"), "--require-main-start"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cemu log paths", result.stdout)

    def test_scene_stage_and_launch_window_negatives(self) -> None:
        result = self.run_gate(
            "--udp-log", str(FIXTURES / "egg_dvd_file_select.log"),
            "--require-good-egg",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("EggStarGalaxy", result.stdout)

        result = self.run_gate(
            "--udp-log", str(FIXTURES / "prior_run_new_run.log"),
            "--from-line", "4", "--require-main-start", "--require-single-launch",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        result = self.run_gate(
            "--udp-log", str(FIXTURES / "two_launch_window.log"),
            "--from-line", "1", "--require-single-launch",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one", result.stdout)

        result = self.run_gate(
            "--udp-log", str(FIXTURES / "task5_pass.log"),
            "--require-single-launch",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --from-line", result.stdout)

    def test_source_format_self_check(self) -> None:
        result = self.run_gate("--self-check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("source log format self-check", result.stdout)

    def test_round3_generated_table_controls_and_bypass_fixtures(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            run_json = Path(directory) / "run.json"
            record = self.run_gate(
                "--udp-log", str(FIXTURES / "rescue_pass.log"),
                "--from-line", "4", "--record-run", str(run_json),
            )
            self.assertEqual(record.returncode, 0, record.stdout + record.stderr)
            emitted = self.run_gate("--emit-frame-table", str(run_json))
            self.assertEqual(emitted.returncode, 0, emitted.stdout + emitted.stderr)
            controls = """\
| Control | Action | Observed |
|---|---|---|
| left stick | movement | yes |
| A | jump | yes |
| Y/X | spin | yes |
| L | camera | yes |
| ZL | crouch | yes |
| ZR | star-bit shooting | yes |
"""
            report = Path(directory) / "generated.md"
            report.write_text(emitted.stdout + "\n" + controls, encoding="utf-8")
            passing = self.run_gate(
                "--report", str(report), "--run-json", str(run_json),
                "--require-frame-buckets", "logic_cpu,gx_translate,gpu_wait,audio",
                "--require-frame-percentiles", "p50,p90", "--require-target-ms", "16.7",
                "--require-control-evidence",
            )
            self.assertEqual(passing.returncode, 0, passing.stdout + passing.stderr)

            for fixture, options in (
                ("bare_controls_report.md", ("--require-control-evidence",)),
                ("header_only_buckets_report.md", (
                    "--require-frame-buckets", "logic_cpu,gx_translate,gpu_wait,audio",
                    "--require-frame-percentiles", "p50,p90",
                )),
                ("microseconds_report.md", (
                    "--run-json", str(run_json), "--require-frame-buckets", "logic_cpu,gx_translate,gpu_wait,audio",
                    "--require-frame-percentiles", "p50,p90",
                )),
            ):
                with self.subTest(fixture=fixture):
                    result = self.run_gate("--report", str(FIXTURES / fixture), *options)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
