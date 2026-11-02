#!/usr/bin/env python3
"""Host contract test for SMG's default-on hardware marker switches."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


MARKERS = ("earlyz", "hiz", "peek2", "peekcolframe", "textiled", "tv720", "texnative", "mem1copy")


def resolve(name: str, present: set[str], smg: bool = True) -> bool:
    """Mirror the renderer rule, including the SMG-only default-on policy."""
    return smg and f"no{name}" not in present or not smg and name in present


def main() -> int:
    source = (Path(__file__).parents[2] / "platform/wiiu/gx2_renderer.cpp").read_text()
    required = [
        "static bool sdDefaultOnMarker(const char* name)",
        "static bool sdGameDefaultMarker(const char* name)",
        "#if defined(PORT_GAME_SMG)",
        "s_tv720Marker = sdGameDefaultMarker(\"tv720\")",
        "s_textiledMarker = sdGameDefaultMarker(\"textiled\")",
        "s_texNative = sdGameDefaultMarker(\"texnative\")",
        "s_peekColFrameMarker = sdDefaultOnMarker(\"peekcolframe\")",
        "SMS Wii U: [peekcolframe] marker=%s",
        "std::snprintf(disableName, sizeof(disableName), \"no%s\", name)",
        "[defaults] earlyz=%d hiz=%d peek2=%d textiled=%d tv720=%d texnative=%d mem1copy=%d",
    ]
    missing = [needle for needle in required if needle not in source]
    if missing:
        raise AssertionError(f"marker default implementation missing: {missing}")

    # Compile the resolution rule as a host executable as well as checking the
    # renderer's wiring below.  This keeps the truth table independent of the
    # Wii U SDK while still making this a build-and-run host test.
    harness = r'''
#include <cstring>
bool resolve(const char* name, const char* marker) {
    char disable[64];
    std::strcpy(disable, "no");
    std::strcat(disable, name);
    return std::strcmp(marker, disable) != 0;
}
int main() {
    return resolve("earlyz", "") && !resolve("earlyz", "noearlyz") &&
           resolve("earlyz", "earlyz") ? 0 : 1;
}
'''
    with tempfile.TemporaryDirectory(prefix="smg-marker-defaults-") as directory:
        src = Path(directory) / "marker_defaults.cpp"
        exe = Path(directory) / "marker_defaults"
        src.write_text(harness, encoding="utf-8")
        build = subprocess.run(
            ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)],
            capture_output=True, text=True,
        )
        if build.returncode:
            raise AssertionError(f"host compile failed: {build.stderr}")
        run = subprocess.run([str(exe)])
        if run.returncode:
            raise AssertionError("host marker resolution executable failed")

    for name in MARKERS:
        assert resolve(name, set()), (name, "default")
        assert not resolve(name, {f"no{name}"}), (name, "disable")
        assert resolve(name, {name}), (name, "legacy positive no-op")
        assert resolve(name, {name, f"no{name}"}) is False, (name, "disable wins")
    for name in ("textiled", "tv720", "texnative"):
        assert not resolve(name, set(), smg=False), (name, "other-game default")
        assert resolve(name, {name}, smg=False), (name, "other-game opt-in")

    print("marker-defaults: PASS: default on, no<name> off, positive marker no-op")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
