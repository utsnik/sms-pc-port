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
    VPAD_BUTTON_ZL = 0x0080,
    VPAD_BUTTON_ZR = 0x0040,
    VPAD_BUTTON_L = 0x0020,
    VPAD_BUTTON_R = 0x0010,
};
typedef struct VPADVec2D { float x, y; } VPADVec2D;
typedef struct VPADVec3D { float x, y, z; } VPADVec3D;
typedef struct VPADTouchData {
    uint16_t x, y, touched, validity;
} VPADTouchData;
enum { VPAD_VALID = 0 };
typedef struct VPADStatus {
    uint32_t hold;
    uint32_t trigger;
    uint32_t release;
    VPADVec2D leftStick;
    VPADVec2D rightStick;
    unsigned char accelerometer_padding[0x1c];
    VPADVec3D gyro;
    unsigned char gyro_padding[0x0e];
    VPADTouchData tpNormal;
} VPADStatus;
extern "C" int32_t VPADRead(VPADChan, VPADStatus*, uint32_t, VPADReadError*);
extern "C" void VPADInit(void);
extern "C" void VPADGetTPCalibratedPoint(VPADChan, VPADTouchData*,
                                           const VPADTouchData*);
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
static uint16_t g_touch_x;
static uint16_t g_touch_y;
static uint16_t g_touch_touched;
static uint16_t g_touch_validity;
static float g_gyro_x;
static float g_gyro_y;
static int g_calibrated_calls;
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
    status->tpNormal.x = g_touch_x;
    status->tpNormal.y = g_touch_y;
    status->tpNormal.touched = g_touch_touched;
    status->tpNormal.validity = g_touch_validity;
    status->gyro.x = g_gyro_x;
    status->gyro.y = g_gyro_y;
    if (error) *error = VPAD_READ_SUCCESS;
    return 1;
}
extern "C" void VPADInit(void) {}
extern "C" void VPADGetTPCalibratedPoint(VPADChan, VPADTouchData* output,
                                            const VPADTouchData* input) {
    ++g_calibrated_calls;
    *output = *input;
}

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

    g_field = 1240;
    status = read_status();
    check(status.hold & WPAD_BUTTON_A, "249-event script reaches final event");
    check(g_log.find("[smg-input] autoplay: 512 scripted events") != std::string::npos,
          "512-event script is fully loaded");
    check(g_log.find("script full") != std::string::npos,
          "512-event script reports capacity");
    check(g_log.find("script larger than 16383 bytes, truncated at the last complete token") !=
              std::string::npos,
          "oversized autoplay script is reported once");
    check(g_log.find("unknown token 'partial_unknown_") == std::string::npos,
          "partial oversized token is dropped");
    check(g_log.find("unknown token 'bogus'") != std::string::npos,
          "unknown token after capacity is reported");

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
    check(status.dpd_valid_fg == 2 && status.pos.x == 0.0f && status.pos.y == 0.0f,
          "script pointer yields to gyro fallback");

    g_vpad_hold = VPAD_BUTTON_A | VPAD_BUTTON_LEFT;
    g_stick_x = -0.75f;
    g_field = 20;
    status = read_status();
    check((status.hold & (WPAD_BUTTON_A | WPAD_BUTTON_LEFT)) ==
              (WPAD_BUTTON_A | WPAD_BUTTON_LEFT), "VPAD button mapping");
    check(status.ex_status.fs.stick.x < -0.7f, "VPAD stick mapping");
    check(status.trig & WPAD_BUTTON_A, "VPAD trigger mapping");

    g_vpad_hold = VPAD_BUTTON_A | VPAD_BUTTON_B | VPAD_BUTTON_X | VPAD_BUTTON_Y |
                  VPAD_BUTTON_ZR | VPAD_BUTTON_ZL | VPAD_BUTTON_L;
    g_stick_x = 0.25f;
    g_stick_y = -0.5f;
    g_touch_touched = 0;
    g_gyro_x = 0.0f;
    g_gyro_y = 0.0f;
    g_field = 25;
    status = read_status();
    check((status.hold & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_1 |
                          WPAD_BUTTON_2 | WPAD_BUTTON_Z | WPAD_BUTTON_C)) ==
              (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_BUTTON_2 |
               WPAD_BUTTON_Z | WPAD_BUTTON_C),
          "GamePad A/ZR/ZL/L/Y/X mapping");
    check(status.ex_status.fs.stick.x > 0.2f && status.ex_status.fs.stick.y < -0.4f,
          "GamePad left-stick mapping");
    std::puts("input-host: PASS: GamePad A/ZR/ZL/L/Y/X and left-stick mappings");

    g_vpad_hold = 0;
    g_touch_x = 427;
    g_touch_y = 240;
    g_touch_touched = 1;
    g_touch_validity = VPAD_VALID;
    g_gyro_x = 0.75f;
    g_gyro_y = -0.75f;
    g_field = 30;
    status = read_status();
    check(g_calibrated_calls > 0, "calibrated touchscreen sample");
    check(status.dpd_valid_fg == 2 && status.pos.x > -0.01f && status.pos.x < 0.01f &&
              status.pos.y > -0.01f && status.pos.y < 0.01f,
          "touchscreen pointer is primary");
    int touch_log_count = 0;
    for (size_t at = 0; (at = g_log.find("hardware pointer source: touch", at)) !=
                           std::string::npos; ++at)
        ++touch_log_count;
    check(touch_log_count == 1, "touch source diagnostic is edge-triggered");

    g_touch_touched = 0;
    g_touch_validity = 1;
    g_gyro_x = 2.0f;
    g_gyro_y = -2.0f;
    g_field = 31;
    status = read_status();
    check(status.dpd_valid_fg == 2 && status.pos.x == 1.0f && status.pos.y == -1.0f,
          "gyro pointer fallback and clamp");
    check(status.horizon.x == 1.0f && status.horizon.y == 0.0f,
          "neutral pointer horizon");
    g_field = 32;
    KPADStatus five_frame_status{};
    for (int frame = 0; frame < 5; ++frame) {
        five_frame_status = read_status();
        check(five_frame_status.dpd_valid_fg >= 2, "five-frame DPD validity");
    }
    g_vpad_hold = VPAD_BUTTON_R;
    g_gyro_x = 0.25f;
    g_gyro_y = -0.25f;
    g_field = 38;
    status = read_status();
    check(status.pos.x == 0.0f && status.pos.y == 0.0f &&
              (status.hold & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_Z |
                              WPAD_BUTTON_C)) == 0,
          "R recentres gyro without Wii button");
    int gyro_log_count = 0;
    for (size_t at = 0; (at = g_log.find("hardware pointer source: gyro", at)) !=
                           std::string::npos; ++at)
        ++gyro_log_count;
    check(gyro_log_count == 1, "gyro source diagnostic is edge-triggered");
    g_vpad_hold = VPAD_BUTTON_A;
    g_field = 39;
    status = read_status();
    check(status.trig & WPAD_BUTTON_A, "pointer A trigger edge");
    g_field = 40;
    status = read_status();
    check((status.hold & WPAD_BUTTON_A) && !(status.trig & WPAD_BUTTON_A),
          "pointer A is not a repeating trigger");
    std::puts("input-host: PASS: touchscreen pointer and gyro fallback; five-frame DPD validity");

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
        extended_script = ",".join(f"a@{1000 + index}+1" for index in range(505))
        (root / "smg-autoplay").write_text(
            "a@2+3,b@2+3,up@5+2,stick_right@7+2,pointer@10,2,-2+4,"
            "b@90+2,stick_left@95+2,"
            "pointer@12,0.75,up@15+1,unknown@1+2," + extended_script +
            ",bogus@1,partial_unknown_" + "x" * 20000 + "\n",
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
    parser.add_argument("--require-stage3-gamepad", action="store_true")
    parser.add_argument("--require-touch-pointer", action="store_true")
    parser.add_argument("--require-gyro-pointer", action="store_true")
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
    if args.require_stage3_gamepad:
        required = ("VPAD_BUTTON_ZR", "VPAD_BUTTON_ZL", "VPAD_BUTTON_L", "VPAD_BUTTON_R")
        missing = [token for token in required if token not in source]
        if missing:
            return fail("missing Stage 3 GamePad controls: " + ", ".join(missing))
    if args.require_touch_pointer:
        required = ("VPADGetTPCalibratedPoint", "VPAD_VALID", "touch.touched")
        missing = [token for token in required if token not in source]
        if missing:
            return fail("missing touchscreen pointer path: " + ", ".join(missing))
    if args.require_gyro_pointer:
        required = ("vpad.gyro", "dpd_valid_fg", "clamp_normalized")
        missing = [token for token in required if token not in source]
        if missing:
            return fail("missing gyro pointer path: " + ", ".join(missing))
    try:
        output = host_check(args.provider)
    except RuntimeError as exc:
        return fail(str(exc))
    print("input: PASS: connected Wii Remote+Nunchuk; A/B/stick autoplay mapped")
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
