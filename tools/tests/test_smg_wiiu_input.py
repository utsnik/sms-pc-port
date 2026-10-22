#!/usr/bin/env python3
"""Host-test the Galaxy-only KPAD/WPAD provider and its autoplay script."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> int:
    print(f"input: FAIL: {message}")
    return 1


HOST_WPAD = r"""
#pragma once
#include <stdint.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int BOOL;
typedef float f32;

typedef void (*WPADExtensionCallback)(s32, s32);
typedef void (*WPADConnectCallback)(s32, s32);
typedef void (*WPADCallback)(s32, s32);
typedef void (*WPADAlloc)(u32);
typedef u8 (*WPADFree)(void*);
typedef void (*WPADSamplingCallback)(s32);

typedef struct Vec { f32 x, y, z; } Vec;
typedef struct Vec2 { f32 x, y; } Vec2;

typedef struct WPADInfo {
    BOOL dpd, speaker, attach, lowBat, nearempty;
    u8 battery, led, protocol, firmware;
} WPADInfo;
typedef struct WPADAcc { int16_t x, y, z; } WPADAcc;

enum {
    WPAD_CHAN0 = 0,
    WPAD_MAX_CONTROLLERS = 4,
    WPAD_MOTOR_STOP = 0,
    WPAD_MOTOR_RUMBLE = 1,
    WPAD_DEV_CORE = 0,
    WPAD_DEV_FREESTYLE = 1,
    WPAD_DEV_NOT_FOUND = 253,
    WPAD_DEV_UNKNOWN = 255,
    WPAD_ERR_NONE = 0,
    WPAD_ERR_NO_CONTROLLER = -1,
    WPAD_BUTTON_LEFT = 0x0001,
    WPAD_BUTTON_RIGHT = 0x0002,
    WPAD_BUTTON_DOWN = 0x0004,
    WPAD_BUTTON_UP = 0x0008,
    WPAD_BUTTON_PLUS = 0x0010,
    WPAD_BUTTON_2 = 0x0100,
    WPAD_BUTTON_1 = 0x0200,
    WPAD_BUTTON_B = 0x0400,
    WPAD_BUTTON_A = 0x0800,
    WPAD_BUTTON_MINUS = 0x1000,
    WPAD_BUTTON_Z = 0x2000,
    WPAD_BUTTON_C = 0x4000,
    WPAD_BUTTON_HOME = 0x8000,
    WPAD_NUNCHUK_BUTTON_Z = 0x2000,
    WPAD_NUNCHUK_BUTTON_C = 0x4000,
    WPAD_FMT_NUNCHUK_ACC_DPD = 5,
    WPAD_BATTERY_LEVEL_MAX = 4,
    WPAD_SENSOR_BAR_POS_TOP = 1,
};

typedef int32_t WPADChan;
typedef int32_t WPADExtensionType;

extern "C" {
WPADExtensionCallback WPADSetExtensionCallback(s32, WPADExtensionCallback);
WPADConnectCallback WPADSetConnectCallback(s32, WPADConnectCallback);
s32 WPADProbe(s32, u32*);
}
"""


HOST_KPAD = r"""
#pragma once
#include <stdint.h>
#include <revolution/wpad.h>

typedef struct KPADStatus {
    uint32_t hold, trig, release;
    Vec acc;
    float acc_value, acc_speed;
    Vec2 pos, vec;
    float speed;
    Vec2 horizon, hori_vec;
    float hori_speed, dist, dist_vec, dist_speed;
    Vec2 acc_vertical;
    uint8_t dev_type;
    int8_t wpad_err;
    int8_t dpd_valid_fg;
    uint8_t data_format;
    struct {
        struct {
            Vec2 stick;
            Vec acc;
            float acc_value;
            float acc_speed;
        } fs;
    } ex_status;
} KPADStatus;

typedef int KPADError;
enum {
    KPAD_ERROR_OK = 0,
    KPAD_ERROR_NO_SAMPLES = -1,
};

extern "C" {
void KPADInit(void);
void KPADReset(void);
void KPADSetAccParam(int32_t, float, float);
void KPADSetBtnRepeat(int32_t, float, float);
void KPADSetDistParam(int32_t, float, float);
void KPADSetHoriParam(int32_t, float, float);
void KPADSetPosParam(int32_t, float, float);
void KPADSetSensorHeight(int32_t, float);
s32 KPADRead(int32_t, KPADStatus*, uint32_t);
uint32_t KPADReadEx(int32_t, KPADStatus*, uint32_t, KPADError*);
}
"""


HOST_VPAD = r"""
#pragma once
#include <stdint.h>

typedef int VPADChan;
typedef enum VPADReadError {
    VPAD_READ_SUCCESS = 0,
    VPAD_READ_NO_SAMPLES = -1,
} VPADReadError;
enum {
    VPAD_CHAN_0 = 0,
    VPAD_BUTTON_A = 0x8000,
    VPAD_BUTTON_B = 0x4000,
    VPAD_BUTTON_X = 0x2000,
    VPAD_BUTTON_Y = 0x1000,
    VPAD_BUTTON_LEFT = 0x0800,
    VPAD_BUTTON_RIGHT = 0x0400,
    VPAD_BUTTON_UP = 0x0200,
    VPAD_BUTTON_DOWN = 0x0100,
    VPAD_BUTTON_PLUS = 0x0008,
    VPAD_BUTTON_MINUS = 0x0004,
    VPAD_BUTTON_HOME = 0x0002,
};
typedef struct VPADVec2D { float x, y; } VPADVec2D;
typedef struct VPADStatus {
    uint32_t hold;
    VPADVec2D leftStick;
    VPADVec2D rightStick;
} VPADStatus;
extern "C" int32_t VPADRead(VPADChan, VPADStatus*, uint32_t, VPADReadError*);
extern "C" void VPADInit(void);
"""


HARNESS = r"""
#include "input.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>

static uint32_t g_field;
static uint32_t g_vpad_hold;
static float g_stick_x;
static float g_stick_y;
static std::string g_root;
static std::string g_log;
static volatile unsigned char g_stack_sink;

extern "C" const char* sms_wiiu_app_root(void) { return g_root.c_str(); }
extern "C" uint32_t sms_wiiu_frame_count(void) { return g_field; }
extern "C" void port_log(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_log += line;
}

extern "C" int32_t VPADRead(VPADChan, VPADStatus* status, uint32_t count,
                              VPADReadError* error) {
    if (!status || !count) {
        if (error) *error = VPAD_READ_NO_SAMPLES;
        return 0;
    }
    std::memset(status, 0, sizeof(*status));
    status->hold = g_vpad_hold;
    status->leftStick.x = g_stick_x;
    status->leftStick.y = g_stick_y;
    if (error) *error = VPAD_READ_SUCCESS;
    return 1;
}
extern "C" void VPADInit(void) {}

static void fail(const char* message) {
    std::fprintf(stderr, "input-host: FAIL: %s\n", message);
    std::exit(1);
}

static void check(bool condition, const char* message) {
    if (!condition) fail(message);
}

static void clobber_stack() {
    volatile unsigned char padding[8192];
    for (size_t i = 0; i < sizeof(padding); ++i)
        padding[i] = static_cast<unsigned char>(i);
    g_stack_sink = padding[0];
}

static KPADStatus read_status() {
    KPADStatus status{};
    KPADError error = KPAD_ERROR_NO_SAMPLES;
    check(KPADReadEx(WPAD_CHAN0, &status, 1, &error) == 1, "KPADReadEx sample");
    check(error == KPAD_ERROR_OK, "KPADReadEx status");
    return status;
}

static int g_extension_calls;
static int g_connect_calls;
static void extension_callback(int32_t chan, int32_t result) {
    check(chan == WPAD_CHAN0 && result == WPAD_DEV_FREESTYLE, "extension callback");
    ++g_extension_calls;
}
static void connect_callback(int32_t chan, int32_t result) {
    check(chan == WPAD_CHAN0 && result == WPAD_ERR_NONE, "connect callback");
    ++g_connect_calls;
}

int main(int argc, char** argv) {
    check(argc == 2, "usage");
    g_root = std::string(argv[1]) + "/";
    KPADInit();
    WPADSetConnectCallback(WPAD_CHAN0, connect_callback);
    WPADSetExtensionCallback(WPAD_CHAN0, extension_callback);

    uint32_t type = WPAD_DEV_NOT_FOUND;
    check(WPADProbe(WPAD_CHAN0, &type) == WPAD_ERR_NONE, "connected WPADProbe");
    check(type == WPAD_DEV_FREESTYLE, "Nunchuk WPAD type");

    g_field = 2;
    KPADStatus status = read_status();
    check(g_connect_calls == 1 && g_extension_calls == 1, "callbacks once");
    check(status.wpad_err == WPAD_ERR_NONE && status.dev_type == WPAD_DEV_FREESTYLE,
          "connected KPAD status");
    check(status.hold & WPAD_BUTTON_A, "A hold");
    check(status.hold & WPAD_BUTTON_B, "B hold");
    check(status.trig & WPAD_BUTTON_A && status.trig & WPAD_BUTTON_B, "A+B trigger edge");

    g_field = 3;
    status = read_status();
    check((status.hold & (WPAD_BUTTON_A | WPAD_BUTTON_B)) ==
              (WPAD_BUTTON_A | WPAD_BUTTON_B), "A+B simultaneous hold");
    check((status.trig & (WPAD_BUTTON_A | WPAD_BUTTON_B)) == 0, "hold has no repeat trigger");

    clobber_stack();
    for (g_field = 21; g_field < 100; ++g_field)
        status = read_status();
    check(g_log.find("[smg-input] autoplay: press b field 90") != std::string::npos,
          "post-clobber button log uses owned name");
    check(g_log.find("[smg-input] autoplay: press stick_left field 95") != std::string::npos,
          "post-clobber stick log uses owned name");

    g_field = 5;
    status = read_status();
    check((status.hold & WPAD_BUTTON_UP) && (status.trig & WPAD_BUTTON_UP),
          "button script edge");
    check(status.ex_status.fs.stick.x == 0.0f, "neutral Nunchuk stick");

    g_field = 7;
    status = read_status();
    check(status.ex_status.fs.stick.x > 0.9f, "stick right script");

    g_field = 10;
    status = read_status();
    check(status.dpd_valid_fg == 2 && status.pos.x == 1.0f && status.pos.y == -1.0f,
          "pointer validity and clamped position");

    g_field = 14;
    status = read_status();
    check(status.dpd_valid_fg == 0, "pointer hold window");

    g_vpad_hold = VPAD_BUTTON_A | VPAD_BUTTON_LEFT;
    g_stick_x = -0.75f;
    g_field = 20;
    status = read_status();
    check((status.hold & (WPAD_BUTTON_A | WPAD_BUTTON_LEFT)) ==
              (WPAD_BUTTON_A | WPAD_BUTTON_LEFT), "VPAD button mapping");
    check(status.ex_status.fs.stick.x < -0.7f, "VPAD stick mapping");
    check(status.trig & WPAD_BUTTON_A, "VPAD trigger mapping");

    check(g_log.find("[smg-input] autoplay: press a field 2") != std::string::npos,
          "exact button log after script buffer lifetime");
    check(g_log.find("[smg-input] autoplay: pointer field 10") != std::string::npos,
          "exact pointer log after script buffer lifetime");
    check(g_log.find("[smg-input] autoplay: malformed pointer directive 'pointer@12'") !=
              std::string::npos,
          "malformed pointer log");
    g_field = 15;
    status = read_status();
    check(status.hold & WPAD_BUTTON_UP, "malformed pointer does not swallow next directive");
    check(g_log.find("[smg-input] autoplay: pointer x 2.000000 clamped to 1.000000") !=
              std::string::npos,
          "pointer x clamp log");
    check(g_log.find("[smg-input] autoplay: pointer y -2.000000 clamped to -1.000000") !=
              std::string::npos,
          "pointer y clamp log");
    check(g_log.find("unknown") != std::string::npos, "unknown-token log");
    std::puts("input-host: PASS: real input.cpp; callbacks, edges, pointer, VPAD mapping");
    return 0;
}
"""


def host_check(provider: Path) -> str:
    with tempfile.TemporaryDirectory(prefix="smg-input-") as temporary:
        root = Path(temporary)
        include = root / "include"
        (include / "revolution").mkdir(parents=True)
        (include / "vpad").mkdir()
        (include / "revolution" / "wpad.h").write_text(HOST_WPAD, encoding="utf-8")
        (include / "revolution" / "kpad.h").write_text(HOST_KPAD, encoding="utf-8")
        (include / "vpad" / "input.h").write_text(HOST_VPAD, encoding="utf-8")
        (root / "autopress").write_text("", encoding="utf-8")
        (root / "smg-autoplay").write_text(
            "a@2+3,b@2+3,up@5+2,stick_right@7+2,pointer@10,2,-2+4,"
            "b@90+2,stick_left@95+2,"
            "pointer@12,0.75,up@15+1,unknown@1+2\n",
            encoding="utf-8",
        )
        harness = root / "input_host.cpp"
        harness.write_text(HARNESS, encoding="utf-8")
        executable = root / "input_host"
        compile_result = subprocess.run(
            [
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(include), "-I", str(provider.parent),
                str(provider), str(harness), "-o", str(executable),
            ],
            capture_output=True,
            text=True,
        )
        if compile_result.returncode:
            raise RuntimeError(
                "host harness compile failed:\n"
                + compile_result.stdout + compile_result.stderr
            )
        run_result = subprocess.run(
            [str(executable), str(root)], capture_output=True, text=True
        )
        output = run_result.stdout + run_result.stderr
        if run_result.returncode:
            raise RuntimeError(
                f"host harness failed ({run_result.returncode}):\n{output.rstrip()}"
            )
        return output.rstrip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--provider", type=Path, required=True)
    parser.add_argument("--shared-pad", type=Path, required=True)
    parser.add_argument("--require-connected", action="store_true")
    parser.add_argument("--require-nunchuk", action="store_true")
    parser.add_argument("--require-a-b-stick-script", action="store_true")
    args = parser.parse_args()

    if not args.provider.is_file():
        return fail("missing KPAD/WPAD provider: " + str(args.provider))
    if not args.shared_pad.is_file():
        return fail("missing shared pad reference: " + str(args.shared_pad))
    try:
        source = args.provider.read_text(encoding="utf-8")
        shared = args.shared_pad.read_text(encoding="utf-8")
    except OSError as exc:
        return fail(str(exc))
    if "KPADRead" not in source or "WPADProbe" not in source:
        return fail("provider does not own KPAD/WPAD reads")
    if "std::mutex" in source or "pthread" in source or "thread_local" in source:
        return fail("provider uses forbidden host synchronization or TLS")
    if "platform/wiiu/pad.cpp" in source or "#include \"pad.cpp\"" in source:
        return fail("provider reuses the shared pad implementation")
    if "map_remote" not in shared:
        return fail("shared pad reference is not the expected GamePad mapping source")
    if args.require_connected and "WPAD_DEV_FREESTYLE" not in source:
        return fail("provider has no connected Nunchuk device type")
    if args.require_nunchuk and "WPAD_EXT_NUNCHUK" not in source and "WPAD_DEV_FREESTYLE" not in source:
        return fail("provider has no Nunchuk extension path")
    if args.require_a_b_stick_script:
        required = ("a", "b", "stick_left", "stick_right", "stick_up", "stick_down")
        missing = [token for token in required if token not in source]
        if missing or "pointer" not in source:
            return fail("missing autoplay vocabulary: " + ", ".join(missing + ["pointer"] if missing else ["pointer"]))
    try:
        output = host_check(args.provider)
    except RuntimeError as exc:
        return fail(str(exc))
    print("input: PASS: connected Wii Remote+Nunchuk; A/B/stick autoplay mapped")
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
