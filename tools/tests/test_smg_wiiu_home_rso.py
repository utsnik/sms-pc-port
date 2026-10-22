#!/usr/bin/env python3
"""Verify Stage 2 skips the unavailable HOME-menu RSO link on SMG Wii U.

Per task-7-addendum.md (binding), TARGET_PC already makes the
HomeButtonMenuWrapper.cpp RSO-link path dead code for this target; the real
hazard is the unconditional blocking load of HomeButtonMenuWrapperRSO.rso /
product.sel in the StationedFileInfo.cpp stage-2 table. This checks the
*selected* (preprocessor-resolved) table and function body for the Wii U
target's macro set, not just raw source text, so a stray grep hit outside
the active branch cannot produce a false PASS.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--stationed", type=Path, required=True)
    parser.add_argument("--require-stage2-disabled", action="store_true")
    parser.add_argument("--require-log", action="store_true")
    args = parser.parse_args()

    problems = check_wrapper(args.source) + check_stationed(args.stationed)

    if problems:
        for problem in problems:
            fail(problem)
        return 1

    print("home-rso: PASS: SMG Stage 2 skips RSO link and logs neutral HOME behavior")
    return 0


if __name__ == "__main__":
    sys.exit(main())
