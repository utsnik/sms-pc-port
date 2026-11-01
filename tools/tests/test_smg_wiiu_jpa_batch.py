#!/usr/bin/env python3
"""Compile and exercise the shipped JPA CPU vertex transform on the host."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "games/smg/decomp/src/JSystem/JParticle"
SOURCE = Path(__file__).with_name("smg_wiiu_jpa_batch_host.cpp")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="smg-jpa-batch-") as directory:
        executable = Path(directory) / "jpa_batch_host"
        build = subprocess.run(
            ["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(INCLUDE),
             str(SOURCE), "-o", str(executable)], capture_output=True, text=True
        )
        if build.returncode:
            print("jpa-batch: FAIL (compile)\n" + build.stderr)
            return 1
        run = subprocess.run([str(executable)], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
