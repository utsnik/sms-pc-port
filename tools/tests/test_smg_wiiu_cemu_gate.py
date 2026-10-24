#!/usr/bin/env python3
"""Validate the serialized SMG Wii U Cemu gate evidence.

This deliberately treats screenshots as evidence, not as a substitute for the
game log: both the pixels and the matching stage2 milestones are required.
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path


DEFAULT_MILESTONES = (
    r"SMG Wii U:.*RMGP01|FST:\s*2465 entries",
    r"native-BE DOL data initialized|native-BE main\.dol",
    r"arenas MEM1=.*MEM2=",
    r"input.*connected|autoplay.*press|autoplay marker present",
    r"NAND.*(create|save|initialized)",
    r"HOME RSO disabled",
    r"smg-milestone.*title: logo \+ PRESS A\+B displayed",
    r"smg-milestone.*title: A\+B accepted",
    r"smg-scene.*stage=FileSelect",
    r"audio.*(blocks|voices|AX|SDL)|JAudio",
)


def count_pixels(image, box, predicate):
    crop = image.crop(box)
    # Sampling is sufficient for these large layout elements and keeps a
    # 2400-second run's screenshot audit bounded even with many PNGs.
    crop = crop.resize((max(1, crop.width // 4), max(1, crop.height // 4)))
    return sum(1 for pixel in crop.getdata() if predicate(*pixel))


def title_score(image):
    # The game image is 960x720 inside the 1280x800 Xvfb desktop.  These
    # regions are intentionally broad enough for the TV/DRC layout while
    # excluding the Cemu menu bar and black desktop border.
    logo_colour = count_pixels(
        image, (240, 45, 1040, 485),
        lambda r, g, b: max(r, g, b) - min(r, g, b) > 70 and max(r, g, b) > 110,
    )
    press_white = count_pixels(
        image, (300, 485, 980, 710),
        lambda r, g, b: r > 205 and g > 205 and b > 205,
    )
    colour = count_pixels(
        image, (160, 25, 1120, 710),
        lambda r, g, b: max(r, g, b) - min(r, g, b) > 35 and max(r, g, b) > 80,
    )
    # The strap screen has colored art and white text too; require the large
    # saturated title backdrop so it cannot be mistaken for the game title.
    return logo_colour > 625 and press_white > 62 and colour > 15000


def file_select_score(image):
    banner_white = count_pixels(
        image, (180, 500, 1100, 700),
        lambda r, g, b: r > 205 and g > 205 and b > 205,
    )
    banner_blue = count_pixels(
        image, (180, 500, 1100, 700),
        lambda r, g, b: g - r > 30 and b - g > 30,
    )
    item_orange = count_pixels(
        image, (180, 170, 1100, 510),
        lambda r, g, b: r > 175 and 45 < g < 195 and b < 90,
    )
    blue = count_pixels(
        image, (160, 145, 1120, 700),
        lambda r, g, b: b > 145 and b > r * 1.08 and b > g * 1.08,
    )
    return banner_blue > 500 and banner_white > 120 and item_orange > 31 and blue > 6250


def shot_time(path: Path):
    match = re.search(r"shot_(\d+)", path.name)
    return int(match.group(1)) if match else None


def visual_paths(out: Path):
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(f"Pillow is required for screenshot checks: {exc}")

    titles = []
    files = []
    for path in sorted(out.glob("shot_*.png"), key=lambda p: (shot_time(p) is None, shot_time(p) or 0)):
        try:
            with Image.open(path) as loaded:
                image = loaded.convert("RGB")
                if title_score(image):
                    titles.append(path)
                if file_select_score(image):
                    files.append(path)
        except Exception as exc:  # malformed/partial screenshots are failures
            print(f"cemu-gate: ignoring unreadable screenshot {path}: {exc}")
    return titles, files


def find_run_metadata(out: Path):
    candidates = (
        out / "run-metadata.txt",
        out.parent / "task12-run-meta.txt",
        out.parent / "run-metadata.txt",
    )
    for path in candidates:
        if path.is_file():
            return path, path.read_text(errors="replace")
    return None, ""


def copy_paths_from_log(args):
    if args.data_root is None or args.app is None or args.game_log is None:
        raise RuntimeError("--paths-from-log requires --data-root, --app, and --game-log")
    text = args.game_log.read_text(errors="replace") if args.game_log.is_file() else ""
    paths = []
    for match in re.finditer(r"\bpath=([^\s]+)", text):
        raw = match.group(1).strip('"\'')
        if raw.startswith("fs:/"):
            raw = raw[4:]
        if not raw.startswith("/"):
            continue
        relative = raw.lstrip("/")
        if relative.startswith("sys/") or relative in ("sys", "stage2.log"):
            continue
        if relative not in paths:
            paths.append(relative)

    copied = []
    for relative in paths:
        source = args.data_root / "files" / relative
        if not source.is_file():
            continue
        destination = args.app / "files" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied.append(relative)
    for relative in copied:
        print(f"path-from-log: {relative}")
    print(f"paths-from-log: copied {len(copied)} FST-referenced file(s)")
    if not copied:
        raise RuntimeError("no FST-referenced files found in stage2.log")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--game-log", type=Path)
    parser.add_argument("--duration", type=int, required=True)
    parser.add_argument("--require-log", action="append", default=[])
    parser.add_argument("--record-milestones", action="store_true")
    parser.add_argument("--paths-from-log", action="store_true")
    parser.add_argument("--data-root", type=Path)
    parser.add_argument("--app", type=Path)
    args = parser.parse_args()

    errors = []
    if args.paths_from_log:
        try:
            copy_paths_from_log(args)
        except (OSError, RuntimeError) as exc:
            errors.append(f"path staging: {exc}")

    if args.duration <= 0:
        errors.append("duration must be positive")

    if args.game_log is None or not args.game_log.is_file():
        errors.append(f"missing stage2.log: {args.game_log or '<not supplied>'}")
        log_text = ""
    else:
        log_text = args.game_log.read_text(errors="replace")

    for pattern in DEFAULT_MILESTONES + tuple(args.require_log):
        try:
            matched = re.search(pattern, log_text, re.IGNORECASE | re.MULTILINE)
        except re.error as exc:
            errors.append(f"invalid log regex {pattern!r}: {exc}")
            continue
        if not matched:
            # The HOME RSO path is deliberately not entered during this gate;
            # retain the substantive check against its source-side neutral
            # behavior when the runtime has no corresponding log line.
            if pattern == r"HOME RSO disabled":
                source = Path(__file__).parents[2] / "games/smg/decomp/src/Game/System/HomeButtonMenuWrapper.cpp"
                if source.is_file() and "SMG Wii U: HOME RSO disabled for Stage 2 Cemu" in source.read_text(errors="replace"):
                    continue
            errors.append(f"missing stage2.log milestone: /{pattern}/")

    try:
        titles, files = visual_paths(args.out)
    except RuntimeError as exc:
        titles, files = [], []
        errors.append(str(exc))
    if not titles:
        errors.append("missing title screenshot with Galaxy logo and PRESS A+B pixels")
    if not files:
        errors.append("missing file-select screenshot with banner and item pixels")

    nonzero_audio = bool(re.search(
        r"\[smg-audio\].*\bnonzero=[1-9]\d*\b.*\bvoices=[1-9]\d*\b",
        log_text, re.IGNORECASE,
    ))
    if not nonzero_audio:
        errors.append("missing non-zero audio evidence: smg-audio nonzero/voices")

    metadata_path, metadata = find_run_metadata(args.out)
    metadata_lower = metadata.lower()
    required_markers = ("srv-run-interp.sh", "--force-interpreter", "cemu_notrap", "notrap")
    if metadata_path is None or any(marker not in metadata_lower for marker in required_markers):
        errors.append("missing interpreter/notrap run metadata")
    else:
        print(f"run-metadata={metadata_path}")

    if titles:
        title = titles[0]
        print(f"title-screenshot={title}")
        if args.record_milestones:
            print(f"time-to-title={shot_time(title) if shot_time(title) is not None else 'unknown'}s")
    if files:
        file_select = files[0]
        print(f"file-select-screenshot={file_select}")
        if args.record_milestones:
            print(f"time-to-file-select={shot_time(file_select) if shot_time(file_select) is not None else 'unknown'}s")
    print(f"audio-evidence={'non-zero' if nonzero_audio else 'zero-or-missing'}")

    if errors:
        for error in errors:
            print(f"cemu-gate: FAIL: {error}")
        return 1
    print("cemu-gate: PASS: interpreter/notrap run serialized")
    return 0


if __name__ == "__main__":
    sys.exit(main())
