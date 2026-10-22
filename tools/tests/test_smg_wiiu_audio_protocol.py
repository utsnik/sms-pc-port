#!/usr/bin/env python3
"""Check Galaxy's JAudio2 DSP protocol against Sunshine's PROTOCOL.md.

Facts are read from the sources themselves (Galaxy's JAudio2 files and the
HLE's PROTOCOL.md), then compared with the rows of the comparison report
(games/smg/platform/wiiu/SMG_AUDIO_PROTOCOL.md). Every source fact that differs
must be a "difference" row whose fix names an SMG-guarded change; with
--require-complete every required element must be covered and the host
protocol test must exist.
"""

from pathlib import Path
import argparse
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
HOST_TEST = ROOT / "platform/audio/tests/smg_galaxy_protocol_test.cc"
HOST_MAKEFILE = ROOT / "platform/audio/tests/Makefile"
STATUSES = {"match", "compatible", "difference"}
REQUIRED = (
    "mail-prefixes", "boot-handshake", "send-framing", "release-halt", "acks",
    "cmd-0x81", "cmd-0x82", "cmd-0x8E", "cmd-4ch", "frame-size", "sample-rate",
    "output-order", "voice-struct", "mix-sends", "bus-table", "send-table",
    "fx-block", "format-codes", "dsp-cursor", "start-offset", "mixer-level",
    "aram-provider",
)
GUARD = "PORT_GAME_SMG"


def fail(message: str) -> int:
    print(f"audio-protocol: FAIL: {message}")
    return 1


def hex_list(text: str):
    return [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", text)]


def table_after(text: str, name: str):
    m = re.search(re.escape(name) + r"[^{]*\{([^}]*)\}", text)
    return hex_list(m.group(1)) if m else None


def report_rows(text: str):
    rows = {}
    in_table = False
    for line in text.splitlines():
        # Markdown cells; "\|" inside a cell is an escaped pipe, not a border.
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]
        if line.startswith("| ID |"):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            in_table = False
            continue
        if set(line.replace("|", "").strip()) <= {"-", " ", ":"}:
            continue
        if len(cells) < 6:
            continue
        rid = cells[0].strip("`")
        rows[rid] = {"status": cells[4].strip("*` ").lower(), "fix": cells[5]}
    return rows


def smg_guarded_lines(path: Path):
    """Line numbers inside a true PORT_GAME_SMG preprocessor branch."""
    guarded = set()
    stack = []  # [kind, in_else]; kind is "pos", "neg" or None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        directive = line.strip()
        if directive.startswith("#if"):
            kind = None
            if GUARD in directive:
                negative = directive.startswith("#ifndef") or re.search(r"!\s*defined\s*\(?\s*" + GUARD, directive)
                kind = "neg" if negative else "pos"
            stack.append([kind, False])
        elif directive.startswith("#elif") or directive.startswith("#else"):
            if stack:
                stack[-1][1] = True
        elif directive.startswith("#endif"):
            if stack:
                stack.pop()
        elif any((k == "pos" and not e) or (k == "neg" and e) for k, e in stack):
            guarded.add(number)
    return guarded


def check_fix(row_id: str, fix: str):
    refs = re.findall(r"`?([A-Za-z0-9_./-]+)#([^`\s,;]+)`?", fix)
    if not refs:
        return f"{row_id}: difference without a fix reference (path#tag)"
    for rel, tag in refs:
        path = ROOT / rel
        if not path.exists():
            return f"{row_id}: fix file {rel} is absent"
        text = path.read_text(encoding="utf-8")
        if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
            marker = f"smg-audio: {tag}"
            lines = [n for n, l in enumerate(text.splitlines(), 1) if marker in l]
            if not lines:
                return f"{row_id}: {rel} has no '{marker}' marker"
            # Shared platform code must keep Sunshine's build unchanged.
            guarded = smg_guarded_lines(path) if rel.startswith("platform/") else None
            if guarded is not None and any(n not in guarded for n in lines):
                return f"{row_id}: '{marker}' in {rel} is outside a {GUARD} guard"
        elif tag not in text:
            return f"{row_id}: {rel} does not contain {tag}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--protocol", required=True, type=Path)
    parser.add_argument("--galaxy", required=True, nargs="+", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()

    galaxy = {p.name: p for p in args.galaxy}
    need = ("JASDSPInterface.cpp", "JASAudioThread.cpp", "JASDSPChannel.cpp", "dspproc.cpp", "dsptask.cpp")
    missing = [n for n in need if n not in galaxy or not galaxy[n].exists()]
    if missing or not args.protocol.exists():
        return fail("sources missing: " + ", ".join(missing or [str(args.protocol)]))
    src = {n: galaxy[n].read_text(encoding="utf-8", errors="replace") for n in need}
    jaudio = galaxy["dspproc.cpp"].parent
    for sibling in ("JASAiCtrl.cpp", "JASAramStream.cpp"):
        path = jaudio / sibling
        if not path.exists():
            return fail(f"Galaxy source {path} missing")
        src[sibling] = path.read_text(encoding="utf-8", errors="replace")
    protocol = args.protocol.read_text(encoding="utf-8")
    if not args.report.exists():
        return fail(f"comparison report {args.report} not written")
    rows = report_rows(args.report.read_text(encoding="utf-8"))
    if not rows:
        return fail("comparison report has no '| ID |' table")

    # Expected status per element, derived from the sources.
    expect = {}
    pbuses = [int(v, 16) for v in re.findall(r"\d+ = (0x[0-9A-F]{4})", protocol)]
    if len(pbuses) != 11:
        return fail("PROTOCOL.md bus address list not found")

    framing = re.search(r"if \(param_2 == 0\) \{\s*param_2 = (\d+);", src["dsptask.cpp"])
    psays = re.search(r"`n == 0` means (\d+) words", protocol)
    if not framing or not psays:
        return fail("count-0 framing not found in dsptask.cpp or PROTOCOL.md")
    expect["send-framing"] = "match" if framing.group(1) == psays.group(1) else "difference"

    groups = re.findall(r"releaseHalt\(([^)]*)\)", src["JASDSPChannel.cpp"])
    expect["release-halt"] = "difference" if len(groups) > 1 else "match"

    expect["mail-prefixes"] = "match" if ("0xdcd1" in src["dsptask.cpp"].lower() and "0xf355" in src["dsptask.cpp"].lower()
                                          and "0xDCD10004" in protocol and "0xF355" in protocol) else "difference"
    expect["boot-handshake"] = "match" if "DspHandShake" in src["dsptask.cpp"] and "DspHandShake" in protocol else "difference"
    acks = "0xFF00) == 0xFF00" in src["JASAudioThread.cpp"] and "finishWork(mail)" in src["JASAudioThread.cpp"]
    expect["acks"] = "match" if acks and "0xF355FF00" in protocol else "difference"

    words = set(re.findall(r"0x(8[0-9A-F])000000", src["dspproc.cpp"]))
    for cmd in ("81", "82"):
        if cmd not in words:
            return fail(f"dspproc.cpp no longer sends 0x{cmd}")
    expect["cmd-0x81"] = "match" if "0x81000000" in protocol else "difference"
    pad = re.search(r"void DsyncFrame2ch[^{]*\{(.*?)\n\}", src["dspproc.cpp"], re.S)
    expect["cmd-0x82"] = "compatible" if pad and "DSPSendCommands2(msgs, 5" in pad.group(1) else "match"
    expect["cmd-0x8E"] = "difference" if "8E" in words and "0x8E" not in protocol else "match"
    expect["cmd-4ch"] = "compatible"

    sub = re.search(r"sSubFrames = 0x0*(\d+);", src["JASAiCtrl.cpp"])
    expect["frame-size"] = "match" if sub and int(sub.group(1)) == 7 and "7 subframes of 80 samples" in protocol else "difference"
    expect["sample-rate"] = "match" if "32028.5f" in src["JASAiCtrl.cpp"] and "32028.5 Hz" in protocol else "difference"
    order = ("imixcopy(endDacBuffer, dacBuffer" in src["JASAiCtrl.cpp"]
             and "+ r27)" in src["JASAiCtrl.cpp"] and "(outB, outA)" in protocol)
    expect["output-order"] = "match" if order else "difference"

    connect = table_after(src["JASDSPInterface.cpp"], "connect_table[12] =")
    expect["bus-table"] = "match" if connect and connect[1:] == pbuses else "difference"
    send = table_after(src["JASDSPInterface.cpp"], "SEND_TABLE[] =")
    if not send or not connect:
        return fail("SEND_TABLE or connect_table not found in JASDSPInterface.cpp")
    expect["send-table"] = "match" if send[:11] == pbuses else "difference"

    blockbytes = table_after(src["JASDSPInterface.cpp"], "COMP_BLOCKBYTES[8] =")
    formats = all(f"{v} = " in protocol for v in (9, 5, 8, 16))
    expect["format-codes"] = "match" if blockbytes and blockbytes[:4] == [9, 5, 8, 16] and formats else "difference"

    stream = src["JASAramStream.cpp"]
    reads_cursor = "->_70 -" in stream and "->_74 +" in stream
    proto_waves = re.search(r"0x070.*wave", protocol, re.I)
    expect["dsp-cursor"] = "difference" if reads_cursor and not proto_waves else "match"
    skip = re.search(r"case 2:\s*case 3:\s*_68 = param_2;", src["JASDSPInterface.cpp"])
    expect["start-offset"] = "difference" if skip else "match"
    expect["mixer-level"] = "compatible" if "DSP_MIXERLEVEL" in src["dspproc.cpp"] else "match"
    expect["aram-provider"] = "difference" if "DsetVARAM(JKRHeap::getAltAramStartAdr())" in src["JASDSPInterface.cpp"] else "match"

    problems = []
    for rid, row in rows.items():
        if row["status"] not in STATUSES:
            problems.append(f"{rid}: status '{row['status']}' is not one of {sorted(STATUSES)}")
    for rid, want in expect.items():
        row = rows.get(rid)
        if row is None:
            continue
        if want == "difference" and row["status"] != "difference":
            problems.append(f"{rid}: the sources differ but the report says {row['status']}")
        if want != "difference" and row["status"] == "difference":
            problems.append(f"{rid}: the sources agree ({want}) but the report lists a difference")
    for rid, row in rows.items():
        if row["status"] == "difference":
            err = check_fix(rid, row["fix"])
            if err:
                problems.append(err)
    if problems:
        return fail("; ".join(problems))

    differences = sorted(rid for rid, row in rows.items() if row["status"] == "difference")
    if args.require_complete:
        absent = [rid for rid in REQUIRED if rid not in rows]
        if absent:
            return fail("report does not cover: " + ", ".join(absent))
        if not HOST_TEST.exists() or "audio-protocol-test: PASS: non-silent output" not in HOST_TEST.read_text(encoding="utf-8"):
            return fail("host protocol test platform/audio/tests/smg_galaxy_protocol_test.cc is missing")
        if "smg_galaxy_protocol_test:" not in HOST_MAKEFILE.read_text(encoding="utf-8"):
            return fail("platform/audio/tests/Makefile does not build smg_galaxy_protocol_test")
        print("audio-protocol: PASS: comparison complete; differences=%d (%s)" % (len(differences), ", ".join(differences)))
    else:
        print("audio-protocol: PASS: rows consistent with the sources; differences=%d" % len(differences))
    return 0


if __name__ == "__main__":
    sys.exit(main())
