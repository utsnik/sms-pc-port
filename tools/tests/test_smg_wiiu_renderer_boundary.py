#!/usr/bin/env python3
"""Verify SMG selects exactly one Sunshine GX2 renderer owner."""

from pathlib import Path
import argparse
import re
import sys


def fail(message: str) -> str:
    return f"renderer-boundary: FAIL: {message}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--stub", type=Path, required=True)
    parser.add_argument("--require-base-renderer", action="store_true")
    parser.add_argument("--require-one-gx-owner", action="store_true")
    parser.add_argument("--require-no-stage4-effects", action="store_true")
    args = parser.parse_args()

    paths = (args.cmake, args.manifest, args.runtime, args.stub)
    missing = [str(path) for path in paths if not path.exists()]
    if missing:
        print(fail("missing input: " + ", ".join(missing)))
        return 1

    cmake = args.cmake.read_text(encoding="utf-8")
    manifest = args.manifest.read_text(encoding="utf-8")
    runtime = args.runtime.read_text(encoding="utf-8")
    stub = args.stub.read_text(encoding="utf-8")
    smg_branch = cmake.split("set(SMS_WIIU_BUILD_DIR", 1)[0]
    errors = []

    base_renderer = (
        "../../platform/wiiu/gx_stub.cpp",
        "../../platform/wiiu/gx2_texture_pack.cpp",
        "../../platform/wiiu/gx2_texture.cpp",
        "../../platform/wiiu/gx2_renderer.cpp",
        "../../platform/gx/src/gx_texture_pack_common.cpp",
    )
    missing_base = [source for source in base_renderer if source not in smg_branch]
    if args.require_base_renderer and missing_base:
        errors.append("SMG lacks " + ", ".join(path.rsplit("/", 1)[-1] for path in missing_base))

    if "gx_link_stub.cpp" in smg_branch or "gx_link_stub.cpp" in manifest:
        errors.append("SMG still selects gx_link_stub.cpp")

    if re.search(r"void\s+sms_wiiu_gfx_frame\s*\([^)]*\)\s*\{\s*\}", runtime):
        errors.append("Galaxy runtime still defines no-op sms_wiiu_gfx_init()/sms_wiiu_gfx_frame()")
    if re.search(r"int\s+sms_wiiu_gfx_is_initialized\s*\([^)]*\)\s*\{\s*return\s+0\s*;\s*\}", runtime):
        errors.append("Galaxy runtime still owns no-op graphics initialization")
    if "smg_wiiu_gx_link_stub_init" in runtime or "smg_wiiu_gx_link_stub_init" in smg_branch:
        errors.append("SMG still calls the GX link stub initializer")

    main_path = args.runtime.parent / "main.cpp"
    if main_path.exists():
        main = main_path.read_text(encoding="utf-8")
        vi = main.find("port_vi_init();")
        gfx = main.find("sms_wiiu_gfx_init();")
        game = main.find("s_game_entry();")
        if not (vi >= 0 and gfx > vi and game > gfx):
            errors.append("SMG entry does not initialize GX after VI and before port_game_main()")

    if args.require_one_gx_owner:
        owners = sum(source in smg_branch for source in ("../../platform/wiiu/gx_stub.cpp", "gx_link_stub.cpp"))
        if owners != 1 or "../../platform/wiiu/gx_stub.cpp" not in smg_branch:
            errors.append("SMG does not select exactly one GX owner")
        else:
            print("renderer-boundary: PASS: SMG selects Sunshine base GX/GX2 exactly once")

    if args.require_no_stage4_effects:
        stage4_units = re.findall(r"[^\s)]+(?:indirect|bloom|efb|peek)[^\s)]*\.(?:c|cc|cpp)", smg_branch, re.IGNORECASE)
        if stage4_units:
            errors.append("SMG selects Stage 4-only effect units: " + ", ".join(stage4_units))
        else:
            print("renderer-boundary: PASS: Stage 4-only indirect/bloom/EFB/peek features remain out of scope")

    if errors:
        for error in errors:
            print(fail(error))
        return 1
    if not args.require_one_gx_owner:
        print("renderer-boundary: PASS: SMG renderer boundary checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
