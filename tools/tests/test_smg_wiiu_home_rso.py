#!/usr/bin/env python3
"""Verify Stage 2 skips the unavailable HOME-menu RSO link on SMG Wii U.

Per task-7-addendum.md (binding), the authoritative check is the *compiled,
linked* smg.elf, not source text: --elf extracts .rodata/.data from the
actual binary (in the same soh-wiiu-builder container used to build it) and
confirms HomeButtonMenuWrapperRSO.rso / product.sel are absent and the
Stage-2-disabled log string is present. The #if evaluator below
(select_text()) over HomeButtonMenuWrapper.cpp / StationedFileInfo.cpp is
only a fast, non-authoritative source pre-check that always runs for quick
feedback; it can be fooled by any conditional-compilation shape its narrow
logic does not model. --require-stage2-disabled and --require-log require
--elf and gate the corresponding half of the ELF check: without --elf they
fail outright, since the source pre-check alone cannot prove what actually
got compiled in.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

BUILDER_IMAGE = "soh-wiiu-builder:latest"
OBJCOPY = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objcopy"

WIIU_DEFINES = {"TARGET_PC", "TARGET_WIIU", "__WIIU__"}

IF_DEFINED_RE = re.compile(r"^\s*#\s*if\s+defined\(\s*(\w+)\s*\)\s*$")
IF_NOT_DEFINED_RE = re.compile(r"^\s*#\s*if\s+!defined\(\s*(\w+)\s*\)\s*$")
IFDEF_RE = re.compile(r"^\s*#\s*ifdef\s+(\w+)\s*$")
IFNDEF_RE = re.compile(r"^\s*#\s*ifndef\s+(\w+)\s*$")
ELSE_RE = re.compile(r"^\s*#\s*else\s*$")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def select_text(source: str, defines: set) -> str:
    """Resolve simple, single-condition #if/#ifdef/#ifndef/#else/#endif
    blocks against `defines`, returning only the text that would survive
    preprocessing for that macro set. Directive lines themselves are
    dropped. This is a deliberately narrow evaluator: it only understands
    the guard shapes actually used in this file (no &&/||, no nesting
    beyond a plain stack, no #elif)."""
    stack = []  # list of bool: whether this level's condition is true
    out_lines = []
    for line in source.splitlines():
        m = IF_DEFINED_RE.match(line) or IFDEF_RE.match(line)
        if m:
            stack.append(m.group(1) in defines)
            continue
        m = IF_NOT_DEFINED_RE.match(line) or IFNDEF_RE.match(line)
        if m:
            stack.append(m.group(1) not in defines)
            continue
        if ELSE_RE.match(line):
            if not stack:
                raise ValueError("unmatched #else")
            stack[-1] = not stack[-1]
            continue
        if ENDIF_RE.match(line):
            if not stack:
                raise ValueError("unmatched #endif")
            stack.pop()
            continue
        if all(stack):
            out_lines.append(line)
    if stack:
        raise ValueError("unbalanced #if/#endif")
    return "\n".join(out_lines)


def extract_function_body(text: str, signature: str) -> str | None:
    start = text.find(signature)
    if start == -1:
        return None
    brace_open = text.find("{", start)
    if brace_open == -1:
        return None
    depth = 0
    i = brace_open
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace_open + 1 : i]
        i += 1
    return None


def fail(message: str) -> int:
    print(f"home-rso: FAIL: {message}")
    return 1


def check_wrapper(source_path: Path) -> list:
    """Return a list of failure messages for HomeButtonMenuWrapper.cpp."""
    problems = []
    try:
        text = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"could not read {source_path}: {exc}"]

    try:
        selected = select_text(text, WIIU_DEFINES)
    except ValueError as exc:
        return [f"{source_path.name}: {exc}"]

    body = extract_function_body(selected, "void RSO::setupRsoHomeButtonMenu()")
    if body is None:
        problems.append(
            f"no RSO::setupRsoHomeButtonMenu() body survives preprocessing "
            f"for the Wii U target in {source_path.name} "
            f"(live load/link is still at {source_path.name}:45)"
        )
        return problems

    if "receiveFile(" in body:
        problems.append(
            f"Wii U build of setupRsoHomeButtonMenu() still calls receiveFile() "
            f"(live RSO load/link at {source_path.name}:45)"
        )

    if "SMG Wii U: HOME RSO disabled for Stage 2 Cemu" not in body:
        problems.append(
            f"setupRsoHomeButtonMenu() does not log "
            f"'SMG Wii U: HOME RSO disabled for Stage 2 Cemu' for the Wii U build"
        )

    return problems


def check_stationed(stationed_path: Path) -> list:
    """Return a list of failure messages for StationedFileInfo.cpp."""
    problems = []
    try:
        text = stationed_path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"could not read {stationed_path}: {exc}"]

    try:
        wiiu_table = select_text(text, WIIU_DEFINES)
        other_table = select_text(text, set())
    except ValueError as exc:
        return [f"{stationed_path.name}: {exc}"]

    for needle in ("HomeButtonMenuWrapperRSO.rso", "product.sel"):
        if needle in wiiu_table:
            problems.append(
                f"Wii U stationed-file table still requests '{needle}' "
                f"(stationed-file request at {stationed_path.name}:923)"
            )

    # Non-Wii-U targets must keep both entries untouched.
    for needle in ("HomeButtonMenuWrapperRSO.rso", "product.sel"):
        if needle not in other_table:
            problems.append(
                f"non-Wii-U stationed-file table lost '{needle}'; it must be "
                f"retained for targets other than TARGET_WIIU"
            )

    return problems


def _extract_rodata_and_data(elf_path: Path) -> bytes:
    """Run objcopy in the builder container, extracting .rodata and .data
    as two separate flat binaries (one objcopy invocation per section --
    combining --only-section flags in a single call would instead emit one
    image spanning both sections with the gap between them zero-filled,
    which can be enormous if they land in different segments) and return
    their concatenated bytes."""
    mount_dir = elf_path.resolve().parent
    elf_in_container = f"{mount_dir}/{elf_path.name}"
    uid_gid = f"{os.getuid()}:{os.getgid()}"
    script = (
        f'set -euo pipefail; OC="{OBJCOPY}"; '
        f'"$OC" --only-section=.rodata -O binary "{elf_in_container}" /tmp/home_rso_rodata.bin; '
        f'"$OC" --only-section=.data -O binary "{elf_in_container}" /tmp/home_rso_data.bin; '
        f"cat /tmp/home_rso_rodata.bin /tmp/home_rso_data.bin"
    )
    cmd = [
        "docker", "run", "--rm", "--network", "none",
        "--user", uid_gid,
        "-v", f"{mount_dir}:{mount_dir}:ro",
        BUILDER_IMAGE, "bash", "-lc", script,
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=120)
    if result.returncode != 0:
        raise RuntimeError(
            f"objcopy failed extracting .rodata/.data from {elf_path}: "
            f"{result.stderr.decode('utf-8', errors='replace').strip()}"
        )
    return result.stdout


def check_elf(elf_path: Path, check_stage2_disabled: bool, check_log: bool) -> list:
    """Authoritative check: extract .rodata and .data from the actual
    *linked* smg.elf (in the same builder container used to build it) and
    confirm the absent/present strings there, not in source. Per
    task-7-addendum.md, this is the check that matters -- the source
    evaluator above is only a fast pre-check and can be fooled by anything
    its narrow #if logic does not model correctly.

    The two halves are independently selectable so --require-stage2-disabled
    and --require-log each gate only their own half of the ELF evidence."""
    if not elf_path.exists():
        return [f"--elf path does not exist: {elf_path}"]

    try:
        data = _extract_rodata_and_data(elf_path)
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
        return [str(exc)]

    problems = []

    if check_stage2_disabled:
        for needle in (b"HomeButtonMenuWrapperRSO.rso", b"/ModuleData/product.sel"):
            if needle in data:
                problems.append(
                    f"linked {elf_path.name} still contains '{needle.decode()}' "
                    f"in .rodata/.data -- the Wii U stationed table still "
                    f"requests the unavailable HOME RSO"
                )

    if check_log:
        if b"SMG Wii U: HOME RSO disabled for Stage 2 Cemu" not in data:
            problems.append(
                f"linked {elf_path.name} does not contain the "
                f"'SMG Wii U: HOME RSO disabled for Stage 2 Cemu' log string in "
                f".rodata/.data"
            )

    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--stationed", type=Path, required=True)
    parser.add_argument(
        "--elf", type=Path,
        help=(
            "Linked smg.elf to check authoritatively (extracted .rodata/.data "
            "in the soh-wiiu-builder container). Required for "
            "--require-stage2-disabled/--require-log to pass, since the "
            "source evaluator alone is only a fast pre-check."
        ),
    )
    parser.add_argument("--require-stage2-disabled", action="store_true")
    parser.add_argument("--require-log", action="store_true")
    args = parser.parse_args()

    # Fast pre-check: narrow source-level #if evaluation. Always runs, but
    # is not authoritative -- see check_elf() and task-7-addendum.md.
    problems = check_wrapper(args.source) + check_stationed(args.stationed)

    if args.elf is not None:
        # With no --require-* flags at all, --elf alone checks both halves.
        no_require_flags = not (args.require_stage2_disabled or args.require_log)
        check_stage2_disabled = args.require_stage2_disabled or no_require_flags
        check_log = args.require_log or no_require_flags
        problems += check_elf(args.elf, check_stage2_disabled, check_log)
    elif args.require_stage2_disabled or args.require_log:
        problems.append(
            "--require-stage2-disabled/--require-log was given without "
            "--elf: the source pre-check alone cannot authoritatively prove "
            "the compiled Wii U table/log (task-7-addendum.md requires "
            "checking the selected/compiled table); pass --elf <smg.elf>"
        )

    if problems:
        for problem in problems:
            fail(problem)
        return 1

    print("home-rso: PASS: SMG Stage 2 skips RSO link and logs neutral HOME behavior")
    return 0


if __name__ == "__main__":
    sys.exit(main())
