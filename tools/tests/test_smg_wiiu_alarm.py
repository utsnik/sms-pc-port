#!/usr/bin/env python3
"""Host test of games/smg/platform/wiiu/alarm.cpp (Galaxy OSAlarm adapter) ticks.

The adapter is compiled on the host against a fake coreinit (real threads):
each armed coreinit alarm fires from its own thread, and a cancelled one may
still deliver one late callback (the dispatch window between coreinit taking
the alarm off its queue and the callback reading the alarm's user data).

timing: with Galaxy's __OSBusClock set as port_os_init sets it (coreinit's
        busClockSpeed), OSSetPeriodicAlarm(OSNanosecondsToTicks(6666667)) (the
        Wii Remote speaker's period) must fire at 150 Hz on the coreinit
        timebase. With the old GameCube 162 MHz clock it fires 1.53x fast;
        --expect-bus-clock lets the red case be shown.
The script also checks that platform/wiiu/os_core.cpp derives __OSBusClock
from coreinit's busClockSpeed for PORT_GAME_SMG.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r"""
#include <revolution/os.h>
#include <revolution/os/OSAlarm.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" void smg_wiiu_OSCreateAlarm(OSAlarm*);
extern "C" void smg_wiiu_OSSetAlarm(OSAlarm*, OSTime, OSAlarmHandler);
extern "C" void smg_wiiu_OSSetPeriodicAlarm(OSAlarm*, OSTime, OSTime, OSAlarmHandler);
extern "C" void smg_wiiu_OSCancelAlarm(OSAlarm*);

// ---- fake coreinit ---------------------------------------------------------
static const uint32_t kBusClock = 248625000u;            // Wii U busClockSpeed
static const double kTimerHz = kBusClock / 4.0;          // coreinit timebase
u32 __OSBusClock;
extern "C" void port_log(const char*, ...) {}

typedef void (*WutCb)(void*, void*);
struct FakeState {
    std::atomic<bool> cancelled{false};
};
static std::mutex g_fake;
static std::map<void*, void*> g_user;                     // alarm -> user data
static std::map<void*, std::shared_ptr<FakeState>> g_state;
static std::vector<std::thread> g_threads;
static std::mutex g_spin;
static std::atomic<bool> g_late{true};

static void fire_loop(void* alarm, std::shared_ptr<FakeState> st, double first_s, double period_s, WutCb cb) {
    std::mt19937 rng(reinterpret_cast<uintptr_t>(alarm) ^ (uintptr_t)period_s);
    std::this_thread::sleep_for(std::chrono::duration<double>(first_s));
    for (;;) {
        bool was_cancelled = st->cancelled.load();
        if (was_cancelled && !g_late.load())
            return;
        // Dispatch window: a cancel or re-arm may land between coreinit
        // dequeuing the alarm and the callback running.
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
        void* user;
        { std::lock_guard<std::mutex> l(g_fake); user = g_user[alarm]; }
        (void)user;
        cb(alarm, nullptr);
        if (was_cancelled || period_s <= 0)
            return;                                        // at most one late call
        std::this_thread::sleep_for(std::chrono::duration<double>(period_s));
    }
}

static void arm(void* alarm, double first_s, double period_s, WutCb cb) {
    auto st = std::make_shared<FakeState>();
    std::lock_guard<std::mutex> l(g_fake);
    auto& old = g_state[alarm];
    if (old) old->cancelled = true;
    old = st;
    g_threads.emplace_back(fire_loop, alarm, st, first_s, period_s, cb);
}

extern "C" {
void fake_create(void* a) __asm__("OSCreateAlarm");
void fake_create(void* a) { std::lock_guard<std::mutex> l(g_fake); auto& s = g_state[a]; if (s) s->cancelled = true; s.reset(); }
int fake_set(void* a, OSTime t, WutCb cb) __asm__("OSSetAlarm");
int fake_set(void* a, OSTime t, WutCb cb) { arm(a, t / kTimerHz, 0, cb); return 1; }
int fake_setp(void* a, OSTime start, OSTime period, WutCb cb) __asm__("OSSetPeriodicAlarm");
int fake_setp(void* a, OSTime, OSTime period, WutCb cb) { arm(a, period / kTimerHz, period / kTimerHz, cb); return 1; }
int fake_cancel(void* a) __asm__("OSCancelAlarm");
int fake_cancel(void* a) { std::lock_guard<std::mutex> l(g_fake); auto& s = g_state[a]; if (s) s->cancelled = true; return 1; }
void fake_setud(void* a, void* u) __asm__("OSSetAlarmUserData");
void fake_setud(void* a, void* u) { std::lock_guard<std::mutex> l(g_fake); g_user[a] = u; }
void* fake_getud(void* a) __asm__("OSGetAlarmUserData");
void* fake_getud(void* a) { std::lock_guard<std::mutex> l(g_fake); return g_user[a]; }
int fake_spin_acq(void*) __asm__("OSUninterruptibleSpinLock_Acquire");
int fake_spin_acq(void*) { g_spin.lock(); return 1; }
int fake_spin_rel(void*) __asm__("OSUninterruptibleSpinLock_Release");
int fake_spin_rel(void*) { g_spin.unlock(); return 1; }
void fake_yield(void) __asm__("OSYieldThread");
void fake_yield(void) { std::this_thread::yield(); }
}

// ---- timing test ------------------------------------------------------------
static OSAlarm g_speaker;
static std::atomic<int> g_ticks{0};
static void speaker(OSAlarm*, OSContext*) { g_ticks++; }

int main(int argc, char** argv) {
    __OSBusClock = argc > 1 ? (u32)std::strtoul(argv[2], nullptr, 10) : kBusClock;

    g_late = false;
    smg_wiiu_OSCreateAlarm(&g_speaker);
    // SpkSpeakerCtrl::setup: OSSetPeriodicAlarm(&sSpeakerAlarm, OSGetTime(), OSNanosecondsToTicks(6666667), ...)
    smg_wiiu_OSSetPeriodicAlarm(&g_speaker, 0, OSNanosecondsToTicks(6666667), speaker);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    smg_wiiu_OSCancelAlarm(&g_speaker);
    const int hz = g_ticks.load();
    std::printf("alarm-host: speaker alarm %d Hz in 1 s (want ~150)\n", hz);
    for (auto& t : g_threads) t.join();
    if (hz < 120 || hz > 160) {
        std::printf("alarm-host: FAIL: Galaxy tick math and the coreinit timebase disagree\n");
        return 1;
    }
    std::printf("alarm-host: timing PASS\n");
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "games/smg/platform/wiiu/alarm.cpp")
    parser.add_argument("--os-core", type=Path, default=ROOT / "platform/wiiu/os_core.cpp")
    parser.add_argument("--expect-bus-clock", type=int,
                        help="run the timing test with this __OSBusClock (red: 162000000)")
    args = parser.parse_args()

    core = args.os_core.read_text(encoding="utf-8")
    m = re.search(r"#if defined\(PORT_GAME_SMG\)[^#]*__OSBusClock\s*=\s*wut_OSGetSystemInfo\(\)->busClockSpeed;", core)
    if not m and args.expect_bus_clock is None:
        print("alarm: FAIL: os_core.cpp does not set Galaxy's __OSBusClock from coreinit's busClockSpeed")
        return 1

    with tempfile.TemporaryDirectory(prefix="smg-alarm-") as tmp:
        harness = Path(tmp) / "alarm_host.cpp"
        harness.write_text(HARNESS, encoding="utf-8")
        exe = Path(tmp) / "alarm_host"
        # Galaxy's revolution headers drag in PowerPC intrinsics that clash
        # with glibc, so the host build gets small headers made from the real
        # ones: the OSAlarm struct and handler type verbatim, and os.h's timer
        # macros (OS_BUS_CLOCK, OS_TIMER_CLOCK, OSNanosecondsToTicks) verbatim.
        rvl = ROOT / "games/smg/decomp/libs/RVL_SDK/include/revolution"
        os_h = (rvl / "os.h").read_text(encoding="utf-8")
        alarm_h = (rvl / "os/OSAlarm.h").read_text(encoding="utf-8")
        macros = [l for l in os_h.splitlines()
                  if re.match(r"#define (OS_TIMER_CLOCK|OSNanosecondsToTicks|OSMillisecondsToTicks)\b", l)]
        if not re.search(r"#define OS_BUS_CLOCK __OSBusClock", os_h) or len(macros) != 3:
            print("alarm: FAIL: cannot find Galaxy's timer macros in revolution/os.h")
            return 1
        struct = re.search(r"typedef void \(\*OSAlarmHandler\).*?\n\nstruct OSAlarm \{.*?\};", alarm_h, re.S)
        if not struct:
            print("alarm: FAIL: cannot find Galaxy's OSAlarm struct")
            return 1
        stub = Path(tmp) / "inc"
        (stub / "revolution/os").mkdir(parents=True)
        (stub / "port").mkdir()
        (stub / "revolution/os.h").write_text(
            "#pragma once\n#include <stdint.h>\ntypedef uint32_t u32; typedef int64_t s64; typedef s64 OSTime;\n"
            "typedef int BOOL;\ntypedef struct OSContext OSContext;\nextern u32 __OSBusClock;\n"
            "#define OS_BUS_CLOCK __OSBusClock\n" + "\n".join(macros) + "\n", encoding="utf-8")
        (stub / "revolution/os/OSAlarm.h").write_text(
            "#pragma once\n#include <revolution/os.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
            "typedef struct OSAlarm OSAlarm;\n" + struct.group(0) + "\n#ifdef __cplusplus\n}\n#endif\n",
            encoding="utf-8")
        (stub / "port/port.h").write_text("#pragma once\nextern \"C\" void port_log(const char*, ...);\n",
                                          encoding="utf-8")
        defs = ["-DTARGET_WIIU=1", "-D__WIIU__=1",
                "-DOSCreateAlarm=smg_wiiu_OSCreateAlarm", "-DOSSetAlarm=smg_wiiu_OSSetAlarm",
                "-DOSSetPeriodicAlarm=smg_wiiu_OSSetPeriodicAlarm", "-DOSCancelAlarm=smg_wiiu_OSCancelAlarm"]
        inc = ["-I", str(stub)]
        build = subprocess.run(["g++", "-std=c++17", "-O1", "-pthread", "-w", *defs, *inc,
                                str(args.source), str(harness), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            print("alarm: FAIL: host build\n" + build.stderr[-1500:])
            return 1
        cmd = [str(exe)] + ([str(args.expect_bus_clock)] if args.expect_bus_clock else [])
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        print((run.stdout + run.stderr).strip())
        if run.returncode:
            print("alarm: FAIL")
            return 1
    print("alarm: PASS: coreinit-consistent ticks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
