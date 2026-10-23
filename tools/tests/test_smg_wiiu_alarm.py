#!/usr/bin/env python3
"""Host test of games/smg/platform/wiiu/alarm.cpp (Galaxy OSAlarm adapter).

The adapter is compiled on the host against a fake coreinit (real threads):
each armed coreinit alarm fires from its own thread, and a cancelled one may
still deliver one late callback (the dispatch window between coreinit taking
the alarm off its queue and the callback reading the alarm's user data).

self:   handlers that cancel / re-arm (periodic -> one-shot) their own alarm
        must return without waiting for their own in-flight handler.
wrap:   deterministic forced wraparound: a callback parked at the adapter's
        lock while 256 armings cycle all records and identities, the last
        landing on its old record; it must not run a handler.
stale:  deterministic: alarm 0's callback is dequeued and held in its
        dispatch window; alarm 0 is cancelled and alarm 1 armed with a 1 s
        deadline; the held callback then proceeds. No handler may run (it
        did when alarm 1 reused alarm 0's coreinit alarm and user data).
race:   several game threads create, arm (one-shot and periodic) and cancel
        alarms on a few game OSAlarm structs (stack-like reuse of slots),
        while callbacks fire. Every handler call must be for a game alarm that
        is armed with that handler at that moment, and none may arrive after
        smg_wiiu_OSCancelAlarm() returned for that alarm (until it is re-armed).
timing: with Galaxy's __OSBusClock set as port_os_init sets it (coreinit's
        busClockSpeed), OSSetPeriodicAlarm(OSNanosecondsToTicks(6666667)) (the
        Wii Remote speaker's period) must fire at 150 Hz on the coreinit
        timebase. With the old GameCube 162 MHz clock it fires 1.53x fast;
        --expect-bus-clock lets the red case be shown.
--mode runs one of them (default all). The script also checks that
platform/wiiu/os_core.cpp derives __OSBusClock from coreinit's busClockSpeed
for PORT_GAME_SMG.
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
#include <condition_variable>
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
extern "C" int smg_wiiu_alarm_live_instances();

// ---- fake coreinit ---------------------------------------------------------
static const uint32_t kBusClock = 248625000u;            // Wii U busClockSpeed
static const double kTimerHz = kBusClock / 4.0;          // coreinit timebase
u32 __OSBusClock;
extern "C" void port_log(const char*, ...) {}

typedef void (*WutCb)(void*, void*);
struct FakeState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> dequeued{false};
};
static std::mutex g_fake;
static std::map<void*, void*> g_user;                     // alarm -> user data
static std::map<void*, std::shared_ptr<FakeState>> g_state;
static std::vector<std::thread> g_threads;
static std::mutex g_spin;
// stale test: the next dequeued callback is held in its dispatch window until
// released (coreinit has taken the alarm off its queue; the callback has not
// read the alarm's user data yet).
static std::mutex g_hold_m;
static std::condition_variable g_hold_cv;
static bool g_hold = false, g_held = false, g_release = false;

// handoff test: hold the fake native arm after the adapter has entered it.
static std::mutex g_handoff_m;
static std::condition_variable g_handoff_cv;
static bool g_handoff_gate = false, g_handoff_entered = false;
static bool g_handoff_release = false, g_handoff_cancel_called = false;

// wrap test: the next callback to take the adapter's spinlock is parked just
// before it gets it (it has read whatever it reads without the lock).
static thread_local bool t_is_cb = false, t_held_once = false;
static std::atomic<bool> g_lockhold{false};
static std::mutex g_lh_m;
static std::condition_variable g_lh_cv;
static bool g_lh_parked = false, g_lh_release = false;

static void fire_loop(void* alarm, std::shared_ptr<FakeState> st, double first_s, double period_s, WutCb cb) {
    std::mt19937 rng(reinterpret_cast<uintptr_t>(alarm) ^ (uintptr_t)period_s);
    std::this_thread::sleep_for(std::chrono::duration<double>(first_s));
    for (;;) {
        // Dequeue and cancellation are one serialized transition: a cancel
        // that returns success guarantees that this callback will not start.
        {
            std::lock_guard<std::mutex> l(g_fake);
            if (st->cancelled.load())
                return;
            st->dequeued = true;
        }
        // Dispatch window: a cancel or re-arm may land between coreinit
        // dequeuing the alarm and the callback running.
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 400));
        {
            std::unique_lock<std::mutex> l(g_hold_m);
            if (g_hold) {
                g_hold = false;
                g_held = true;
                g_hold_cv.notify_all();
                g_hold_cv.wait(l, [] { return g_release; });
            }
        }
        void* user;
        { std::lock_guard<std::mutex> l(g_fake); user = g_user[alarm]; }
        (void)user;
        t_is_cb = true;
        t_held_once = false;
        cb(alarm, nullptr);
        t_is_cb = false;
        st->dequeued = false;
        if (period_s <= 0)
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
int fake_set(void* a, OSTime t, WutCb cb) {
    {
        std::unique_lock<std::mutex> l(g_handoff_m);
        if (g_handoff_gate) {
            g_handoff_entered = true;
            g_handoff_cv.notify_all();
            g_handoff_cv.wait(l, [] { return g_handoff_release; });
        }
    }
    arm(a, t / kTimerHz, 0, cb); return 1;
}
int fake_setp(void* a, OSTime start, OSTime period, WutCb cb) __asm__("OSSetPeriodicAlarm");
int fake_setp(void* a, OSTime, OSTime period, WutCb cb) { arm(a, period / kTimerHz, period / kTimerHz, cb); return 1; }
int fake_cancel(void* a) __asm__("OSCancelAlarm");
int fake_cancel(void* a) {
    {
        std::lock_guard<std::mutex> l(g_handoff_m);
        if (g_handoff_gate) {
            g_handoff_cancel_called = true;
            g_handoff_cv.notify_all();
        }
    }
    std::lock_guard<std::mutex> l(g_fake);
    auto& s = g_state[a];
    if (!s)
        return 1;
    const bool pending = s->dequeued.load();
    s->cancelled = true;
    return pending ? 0 : 1;
}
void fake_setud(void* a, void* u) __asm__("OSSetAlarmUserData");
void fake_setud(void* a, void* u) { std::lock_guard<std::mutex> l(g_fake); g_user[a] = u; }
void* fake_getud(void* a) __asm__("OSGetAlarmUserData");
void* fake_getud(void* a) { std::lock_guard<std::mutex> l(g_fake); return g_user[a]; }
int fake_spin_acq(void*) __asm__("OSUninterruptibleSpinLock_Acquire");
int fake_spin_acq(void*) {
    if (t_is_cb && !t_held_once) {
        t_held_once = true;
        bool expected = true;
        if (g_lockhold.compare_exchange_strong(expected, false)) {
            std::unique_lock<std::mutex> l(g_lh_m);
            g_lh_parked = true;
            g_lh_cv.notify_all();
            g_lh_cv.wait(l, [] { return g_lh_release; });
        }
    }
    g_spin.lock();
    return 1;
}
int fake_spin_rel(void*) __asm__("OSUninterruptibleSpinLock_Release");
int fake_spin_rel(void*) { g_spin.unlock(); return 1; }
void fake_yield(void) __asm__("OSYieldThread");
void fake_yield(void) { std::this_thread::yield(); }
OSTime fake_time(void) __asm__("OSGetTime");
OSTime fake_time(void) {
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return OSTime((long double)ns * kTimerHz / 1e9L);
}
void* fake_self(void) __asm__("OSGetCurrentThread");
void* fake_self(void) { static thread_local char tag; return &tag; }
}

// ---- race test --------------------------------------------------------------
static const int kGameAlarms = 6;
static OSAlarm g_alarms[kGameAlarms];
static std::mutex g_book;
// Handlers a callback may legitimately run with right now: during an arm both
// the previous and the new arming are live; after it returns only the new
// one; after a cancel returns none.
static OSAlarmHandler g_allowed[kGameAlarms][2];
static std::atomic<int> g_bad{0}, g_calls{0};
static std::atomic<int> g_hits[kGameAlarms][2];          // [alarm][handler_a, handler_b]

static void check_call(OSAlarm* a, OSAlarmHandler h) {
    g_calls++;
    int i = int(a - g_alarms);
    std::lock_guard<std::mutex> l(g_book);
    if (i < 0 || i >= kGameAlarms || (g_allowed[i][0] != h && g_allowed[i][1] != h)) {
        if (g_bad++ < 5)
            std::fprintf(stderr, "alarm-host: bad call alarm=%d handler mismatch or not armed\n", i);
    }
}
// The check runs at the end of a deliberately slow handler: a handler still
// running when cancel returns (or when its record is handed to another alarm)
// is caught as well as one that starts late.
static void hit(OSAlarm* a, int h) {
    int i = int(a - g_alarms);
    if (i >= 0 && i < kGameAlarms)
        g_hits[i][h]++;
}
static void handler_a(OSAlarm* a, OSContext*) {
    hit(a, 0);
    std::this_thread::sleep_for(std::chrono::microseconds(150));
    check_call(a, handler_a);
}
static void handler_b(OSAlarm* a, OSContext*) {
    hit(a, 1);
    std::this_thread::sleep_for(std::chrono::microseconds(150));
    check_call(a, handler_b);
}
static void handler_wrap(OSAlarm* a, OSContext*) { hit(a, 1); }

// Deterministic case (MainLoopFramework's per-frame stack alarm): alarm 0's
// callback is dequeued and held; alarm 0 is cancelled and alarm 1 armed with
// a 1 s deadline (a slot reuse candidate); then the held callback proceeds.
// Neither handler may run: alarm 0 was cancelled, alarm 1 is not due.
// Handlers that cancel or re-arm their own alarm (must not wait for themselves).
static std::atomic<int> g_self_done{0};
static void handler_noop(OSAlarm*, OSContext*) {}
static void handler_self_cancel(OSAlarm* a, OSContext*) {
    smg_wiiu_OSCancelAlarm(a);
    g_self_done++;
}
static void handler_self_rearm(OSAlarm* a, OSContext*) {
    smg_wiiu_OSSetAlarm(a, OSTime(kTimerHz * 10.0), handler_noop);
    g_self_done++;
}

// Both handlers cancel the other alarm while both records have a handler in
// flight. Disarm must not wait for the other callback to return.
static std::atomic<int> g_cross_entered{0}, g_cross_done{0};
static void handler_cross_a(OSAlarm* a, OSContext*) {
    g_cross_entered++;
    while (g_cross_entered.load() < 2)
        std::this_thread::yield();
    smg_wiiu_OSCancelAlarm(&g_alarms[5]);
    g_cross_done++;
}
static void handler_cross_b(OSAlarm* a, OSContext*) {
    g_cross_entered++;
    while (g_cross_entered.load() < 2)
        std::this_thread::yield();
    smg_wiiu_OSCancelAlarm(&g_alarms[4]);
    g_cross_done++;
}
static bool cross_cancel_test() {
    g_cross_entered = 0;
    g_cross_done = 0;
    smg_wiiu_OSCreateAlarm(&g_alarms[4]);
    smg_wiiu_OSCreateAlarm(&g_alarms[5]);
    smg_wiiu_OSSetAlarm(&g_alarms[4], OSTime(kTimerHz * 0.001), handler_cross_a);
    smg_wiiu_OSSetAlarm(&g_alarms[5], OSTime(kTimerHz * 0.001), handler_cross_b);
    for (int i = 0; i < 200 && g_cross_done.load() != 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool ok = g_cross_done.load() == 2;
    std::printf("alarm-host: cross-cancel %s\n", ok ? "returned" : "DEADLOCKED");
    if (ok) {
        smg_wiiu_OSCancelAlarm(&g_alarms[4]);
        smg_wiiu_OSCancelAlarm(&g_alarms[5]);
    }
    return ok;
}

static std::atomic<int> g_handoff_hits{0};
static void handler_handoff(OSAlarm*, OSContext*) { g_handoff_hits++; }
static bool handoff_test() {
    g_handoff_hits = 0;
    {
        std::lock_guard<std::mutex> l(g_handoff_m);
        g_handoff_gate = true;
        g_handoff_entered = false;
        g_handoff_release = false;
        g_handoff_cancel_called = false;
    }
    smg_wiiu_OSCreateAlarm(&g_alarms[0]);
    std::atomic<bool> set_done{false};
    std::thread setter([&] {
        smg_wiiu_OSSetAlarm(&g_alarms[0], OSTime(kTimerHz * 0.001), handler_handoff);
        set_done = true;
    });
    {
        std::unique_lock<std::mutex> l(g_handoff_m);
        if (!g_handoff_cv.wait_for(l, std::chrono::seconds(2), [] { return g_handoff_entered; }))
            return false;
    }
    std::atomic<bool> cancel_done{false};
    std::thread canceller([&] {
        smg_wiiu_OSCancelAlarm(&g_alarms[0]);
        cancel_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool cancel_early;
    {
        std::lock_guard<std::mutex> l(g_handoff_m);
        cancel_early = g_handoff_cancel_called;
    }
    {
        std::lock_guard<std::mutex> l(g_handoff_m);
        g_handoff_release = true;
        g_handoff_cv.notify_all();
    }
    setter.join();
    canceller.join();
    {
        std::lock_guard<std::mutex> l(g_handoff_m);
        g_handoff_gate = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    smg_wiiu_OSCancelAlarm(&g_alarms[0]);
    const bool ok = !cancel_early && set_done && cancel_done && g_handoff_hits.load() == 0;
    std::printf("alarm-host: same-alarm set/cancel handoff %s (set=%d cancel=%d hits=%d)\n",
                ok ? "PASS" : "FAIL", int(set_done.load()), int(cancel_done.load()), g_handoff_hits.load());
    return ok;
}

static std::atomic<int> g_late_periodic_hits{0};
static void handler_late_periodic(OSAlarm*, OSContext*) { g_late_periodic_hits++; }
static std::atomic<int> g_one_shot_hits{0};
static void handler_one_shot(OSAlarm*, OSContext*) { g_one_shot_hits++; }
static bool periodic_leak_test() {
    g_late_periodic_hits = 0;
    g_one_shot_hits = 0;
    const int baseline = smg_wiiu_alarm_live_instances();
    for (int n = 0; n < 50; ++n) {
        {
            std::lock_guard<std::mutex> l(g_hold_m);
            g_hold = true;
            g_held = false;
            g_release = false;
        }
        smg_wiiu_OSSetPeriodicAlarm(&g_alarms[0], 0, OSTime(kTimerHz * 0.001), handler_late_periodic);
        {
            std::unique_lock<std::mutex> l(g_hold_m);
            if (!g_hold_cv.wait_for(l, std::chrono::seconds(2), [] { return g_held; }))
                return false;
        }
        smg_wiiu_OSCancelAlarm(&g_alarms[0]);
        {
            std::lock_guard<std::mutex> l(g_hold_m);
            g_release = true;
            g_hold_cv.notify_all();
        }
        for (int i = 0; i < 100 && smg_wiiu_alarm_live_instances() != baseline; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (smg_wiiu_alarm_live_instances() != baseline)
            return false;
    }
    smg_wiiu_OSSetAlarm(&g_alarms[0], OSTime(kTimerHz * 0.001), handler_one_shot);
    for (int i = 0; i < 200 && g_one_shot_hits.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (int i = 0; i < 100 && smg_wiiu_alarm_live_instances() != baseline; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::printf("alarm-host: periodic late-callback instances baseline=%d live=%d hits=%d; one-shot live=%d hits=%d\n",
                baseline, smg_wiiu_alarm_live_instances(), g_late_periodic_hits.load(),
                smg_wiiu_alarm_live_instances(), g_one_shot_hits.load());
    return smg_wiiu_alarm_live_instances() == baseline && g_late_periodic_hits.load() == 0 &&
           g_one_shot_hits.load() == 1;
}
static bool wait_self(int want) {
    for (int i = 0; i < 200 && g_self_done.load() < want; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return g_self_done.load() >= want;
}
static bool self_test() {
    smg_wiiu_OSCreateAlarm(&g_alarms[2]);
    smg_wiiu_OSSetAlarm(&g_alarms[2], OSTime(kTimerHz * 0.001), handler_self_cancel);
    const bool cancel_ok = wait_self(1);
    std::printf("alarm-host: self-cancel from its handler %s\n", cancel_ok ? "returned" : "DEADLOCKED");
    if (!cancel_ok)
        return false;
    smg_wiiu_OSCreateAlarm(&g_alarms[3]);
    smg_wiiu_OSSetPeriodicAlarm(&g_alarms[3], 0, OSTime(kTimerHz * 0.001), handler_self_rearm);
    const bool rearm_ok = wait_self(2);
    std::printf("alarm-host: self-rearm from its handler %s\n", rearm_ok ? "returned" : "DEADLOCKED");
    if (!rearm_ok)
        return false;
    smg_wiiu_OSCancelAlarm(&g_alarms[3]);
    smg_wiiu_OSCancelAlarm(&g_alarms[2]);
    return true;
}

// Forced wraparound: alarm 0's callback is parked at the adapter's lock (after
// any lock-free read); alarm 0 is cancelled; 255 arm/cancel cycles on alarm 1
// cycle every side-table record and identity ring the adapter has (32 records,
// 256 tickets in a7101af); a 256th arming of alarm 1 with a short deadline
// lands on alarm 0's old record and ticket. The parked callback then proceeds
// after the replacement alarm is due: only the replacement's one callback is
// legitimate.
static bool wrap_test() {
    for (auto& r : g_hits) r[0] = r[1] = 0;
    g_lockhold = true;
    smg_wiiu_OSCreateAlarm(&g_alarms[0]);
    smg_wiiu_OSSetAlarm(&g_alarms[0], OSTime(kTimerHz * 0.001), handler_a);
    {
        std::unique_lock<std::mutex> l(g_lh_m);
        if (!g_lh_cv.wait_for(l, std::chrono::seconds(2), [] { return g_lh_parked; })) {
            std::printf("alarm-host: wrap: callback never reached the adapter lock\n");
            return false;
        }
    }
    smg_wiiu_OSCancelAlarm(&g_alarms[0]);
    smg_wiiu_OSCreateAlarm(&g_alarms[1]);
    for (int n = 0; n < 255; ++n) {
        smg_wiiu_OSSetAlarm(&g_alarms[1], OSTime(kTimerHz * 1.0), handler_wrap);
        smg_wiiu_OSCancelAlarm(&g_alarms[1]);
    }
    smg_wiiu_OSSetAlarm(&g_alarms[1], OSTime(kTimerHz * 0.001), handler_wrap);
    { std::lock_guard<std::mutex> l(g_lh_m); g_lh_release = true; g_lh_cv.notify_all(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    smg_wiiu_OSCancelAlarm(&g_alarms[1]);
    const int a0 = g_hits[0][0] + g_hits[0][1], b1 = g_hits[1][0] + g_hits[1][1];
    std::printf("alarm-host: parked callback after 256 armings: alarm0 calls=%d alarm1 calls=%d\n", a0, b1);
    return a0 == 0 && b1 == 1;
}

static bool stale_test() {
    for (auto& r : g_hits) r[0] = r[1] = 0;
    { std::lock_guard<std::mutex> l(g_hold_m); g_hold = true; g_held = false; g_release = false; }
    smg_wiiu_OSCreateAlarm(&g_alarms[0]);
    smg_wiiu_OSSetAlarm(&g_alarms[0], OSTime(kTimerHz * 0.001), handler_a);
    {
        std::unique_lock<std::mutex> l(g_hold_m);
        g_hold_cv.wait(l, [] { return g_held; });
    }
    smg_wiiu_OSCancelAlarm(&g_alarms[0]);
    smg_wiiu_OSCreateAlarm(&g_alarms[1]);
    smg_wiiu_OSSetAlarm(&g_alarms[1], OSTime(kTimerHz * 1.0), handler_b);
    { std::lock_guard<std::mutex> l(g_hold_m); g_release = true; g_hold_cv.notify_all(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    smg_wiiu_OSCancelAlarm(&g_alarms[1]);
    const int a0 = g_hits[0][0] + g_hits[0][1], b1 = g_hits[1][0] + g_hits[1][1];
    std::printf("alarm-host: stale callback after cancel + reuse: alarm0 calls=%d alarm1 calls=%d\n", a0, b1);
    return a0 == 0 && b1 == 0;
}

// One worker per game alarm keeps the "armed with this handler" bookkeeping
// exact; the workers still race each other on the shared side table.
static void race_test() {
    std::vector<std::thread> ws;
    for (int w = 0; w < kGameAlarms; ++w) {
        ws.emplace_back([w] {
            std::mt19937 rng(1000 + w);
            OSAlarm* a = &g_alarms[w];
            auto allow = [w](OSAlarmHandler x, OSAlarmHandler y) {
                std::lock_guard<std::mutex> l(g_book);
                g_allowed[w][0] = x;
                g_allowed[w][1] = y;
            };
            OSAlarmHandler cur = nullptr;
            for (int n = 0; n < 1500; ++n) {
                OSAlarmHandler h = (rng() & 1) ? handler_a : handler_b;
                switch (rng() % 3) {
                case 0:
                    allow(cur, h);
                    smg_wiiu_OSSetAlarm(a, OSTime(kTimerHz * 0.0002), h);
                    cur = h;
                    allow(cur, nullptr);
                    break;
                case 1:
                    allow(cur, h);
                    smg_wiiu_OSSetPeriodicAlarm(a, 0, OSTime(kTimerHz * 0.0002), h);
                    cur = h;
                    allow(cur, nullptr);
                    break;
                default:
                    smg_wiiu_OSCancelAlarm(a);
                    cur = nullptr;
                    allow(nullptr, nullptr);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
            }
            smg_wiiu_OSCancelAlarm(a);
            allow(nullptr, nullptr);
        });
    }
    for (auto& t : ws) t.join();
}

// ---- timing test ------------------------------------------------------------
static OSAlarm g_speaker;
static std::atomic<int> g_ticks{0};
static void speaker(OSAlarm*, OSContext*) { g_ticks++; }

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    __OSBusClock = argc > 2 ? (u32)std::strtoul(argv[2], nullptr, 10) : kBusClock;

    if (mode == "timing")
        goto timing;
    if (mode == "cross" || mode == "all") {
        if (!cross_cancel_test()) {
            std::printf("alarm-host: FAIL: cross-alarm cancellation deadlocked\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        if (mode == "cross") {
            for (auto& t : g_threads) t.join();
            return 0;
        }
    }
    if (mode == "leak" || mode == "all") {
        if (!periodic_leak_test()) {
            std::printf("alarm-host: FAIL: periodic late callback leaked an instance or ran\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        if (mode == "leak") {
            for (auto& t : g_threads) t.join();
            return 0;
        }
    }
    if (mode == "handoff" || mode == "all") {
        if (!handoff_test()) {
            std::printf("alarm-host: FAIL: same-alarm native arm/cancel handoff raced\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        if (mode == "handoff") {
            for (auto& t : g_threads) t.join();
            return 0;
        }
    }
    if (mode == "self" || mode == "all") {
        if (!self_test()) {
            std::printf("alarm-host: FAIL: a handler cancelling/re-arming its own alarm deadlocked\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        std::printf("alarm-host: self PASS\n");
        if (mode == "self") {
            std::fflush(stdout);
            std::_Exit(0);
        }
    }
    if (mode == "wrap") {
        const bool ok = wrap_test();
        std::printf(ok ? "alarm-host: wrap PASS\n"
                       : "alarm-host: FAIL: a parked callback ran a handler after identity wraparound\n");
        std::fflush(stdout);
        std::_Exit(ok ? 0 : 1);
    }
    if (mode == "stale" || mode == "all") {
        if (!stale_test()) {
            std::printf("alarm-host: FAIL: a dequeued callback ran a handler after cancel/reuse\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        std::printf("alarm-host: stale PASS\n");
        if (mode == "stale") {
            std::fflush(stdout);
            std::_Exit(0);
        }
    }
    race_test();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // late callbacks
    if (g_bad.load()) {
        std::printf("alarm-host: FAIL: %d handler call(s) for an alarm not armed with that handler (of %d)\n",
                    g_bad.load(), g_calls.load());
        std::fflush(stdout);
        std::_Exit(1);
    }
    std::printf("alarm-host: race PASS: %d handler calls, all for armed alarms\n", g_calls.load());
    if (mode == "race") {
        for (auto& t : g_threads) t.join();
        return 0;
    }

timing:
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
    parser.add_argument("--mode", choices=("all", "self", "wrap", "stale", "race", "cross", "leak", "handoff", "timing"), default="all")
    parser.add_argument("--expect-bus-clock", type=int,
                        help="run the timing test with this __OSBusClock (red: 162000000)")
    args = parser.parse_args()

    core = args.os_core.read_text(encoding="utf-8")
    m = re.search(r"#if defined\(PORT_GAME_SMG\)[^#]*__OSBusClock\s*=\s*wut_OSGetSystemInfo\(\)->busClockSpeed;", core)
    if not m and args.expect_bus_clock is None and args.mode not in ("race", "stale", "self", "wrap"):
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
        defs = ["-DTARGET_WIIU=1", "-D__WIIU__=1", "-DSMG_ALARM_HOST_TEST=1",
                "-DOSCreateAlarm=smg_wiiu_OSCreateAlarm", "-DOSSetAlarm=smg_wiiu_OSSetAlarm",
                "-DOSSetPeriodicAlarm=smg_wiiu_OSSetPeriodicAlarm", "-DOSCancelAlarm=smg_wiiu_OSCancelAlarm"]
        inc = ["-I", str(stub)]
        build = subprocess.run(["g++", "-std=c++17", "-O1", "-pthread", "-w", *defs, *inc,
                                str(args.source), str(harness), "-o", str(exe)],
                               capture_output=True, text=True)
        if build.returncode:
            print("alarm: FAIL: host build\n" + build.stderr[-1500:])
            return 1
        clock = [str(args.expect_bus_clock)] if args.expect_bus_clock else []
        # wrap runs in a fresh process: it counts the adapter's armings from zero.
        for mode in (("wrap", "all") if args.mode == "all" else (args.mode,)):
            run = subprocess.run([str(exe), mode] + clock, capture_output=True, text=True, timeout=120)
            print((run.stdout + run.stderr).strip())
            if run.returncode:
                print("alarm: FAIL")
                return 1
    print({"all": "alarm: PASS: due-checked side table (no stale, wrapped or self-deadlocking callbacks) and coreinit-consistent ticks",
           "race": "alarm: PASS: no handler call for an alarm not armed with it (threaded stress)",
           "self": "alarm: PASS: a handler can cancel or re-arm its own alarm (no self-wait)",
           "wrap": "alarm: PASS: a parked callback cannot run a handler after identity wraparound",
           "stale": "alarm: PASS: a dequeued callback cannot run a handler after cancel or slot reuse",
           "cross": "alarm: PASS: cross-alarm cancellation does not deadlock",
           "leak": "alarm: PASS: periodic late callbacks release their instances",
           "handoff": "alarm: PASS: same-alarm native arm/cancel is serialized",
           "timing": "alarm: PASS: coreinit-consistent ticks"}[args.mode])
    return 0


if __name__ == "__main__":
    sys.exit(main())
