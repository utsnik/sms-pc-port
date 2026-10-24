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
                "--require-stage2-pass", "--require-main-start",
                "--require-no-notrap", "--require-single-launch",
                "--require-console-frame-split", "--require-good-egg",
                "--require-log", r"SMG Wii U: main entered",
                "--record-run", str(record),
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("stage=EggStarGalaxy (Good Egg) reached", result.stdout)
            evidence = json.loads(record.read_text(encoding="utf-8"))
            self.assertEqual(evidence["run_interval"]["start_line"], 2)
            self.assertEqual(evidence["run_interval"]["main_start_count"], 1)
            self.assertEqual(len(evidence["framesplit"]), 2)
            self.assertTrue(any("EggStarGalaxy" in path for path in evidence["stage_path"]))

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
            (("two_launch.log", "--require-main-start", "--require-single-launch"), "exactly one"),
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

    def test_source_format_self_check(self) -> None:
        result = self.run_gate("--self-check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("source log format self-check", result.stdout)


if __name__ == "__main__":
    unittest.main()
