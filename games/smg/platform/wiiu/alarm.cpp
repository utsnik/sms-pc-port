/* Galaxy OSAlarm adapter (Wii SDK alarm API on Wii U coreinit).
 *
 * Galaxy's OSAlarm (revolution/os/OSAlarm.h) is the Wii's 0x30-byte struct;
 * the Wii SDK names resolve to coreinit's imports, whose OSAlarm is 0x58 bytes.
 * Unadapted, OSCreateAlarm/OSSetPeriodicAlarm on SpkSpeakerCtrl's static
 * sSpeakerAlarm wrote 0x28 bytes past it, over FileRipper's sReadBuffer and
 * sReadBufferEnd, and the next Yaz0 load stored through a null read buffer.
 *
 * Each game alarm gets a coreinit alarm from a side table; the coreinit
 * callback calls the game handler with the game alarm. Ticks pass through
 * unchanged: port_os_init sets Galaxy's __OSBusClock to coreinit's bus clock,
 * so Galaxy's OS_TIMER_CLOCK tick math and OSGetTime are coreinit's units.
 * There is no Wii OSContext for the handler: it receives nullptr (Galaxy's
 * two handlers ignore it).
 *
 * Concurrency: callbacks run on coreinit's alarm context while game threads
 * create, arm and cancel; all adapter state is under one uninterruptible
 * spinlock, and a callback reads nothing before taking it.
 *  - Every arming gets a new coreinit alarm object and a 64-bit generation
 *    cookie. The cookie is immutable for that coreinit alarm and is carried
 *    in its user data, so a callback that was dequeued before cancellation
 *    cannot observe a later arming's user data. Under the lock the callback
 *    checks that its generation is still the record's current generation and
 *    that its arming is due: OSGetTime() >= due, where due is computed from
 *    the same coreinit timebase when the alarm is armed. A one-shot then
 *    disarms; a periodic alarm advances due to its next period.
 *  - Every arming takes a free record (least recently released first);
 *    re-arming or cancelling retires the old one: disarm, OSCancelAlarm,
 *    then wait for its handlers still running, except one running on the
 *    calling thread (a handler cancelling or re-arming its own alarm), which
 *    would otherwise wait for itself. A record with a handler in flight is
 *    never reused.
 */
#include "port/port.h"

#include <revolution/os.h>
#include <revolution/os/OSAlarm.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace {

struct WutAlarm {
    alignas(8) unsigned char bytes[0x58];
};
typedef void (*WutAlarmCallback)(WutAlarm*, void*);
struct WutSpinLock {
    alignas(16) unsigned char bytes[0x10];
};

extern "C" {
void wut_OSCreateAlarm(WutAlarm*) __asm__("OSCreateAlarm");
BOOL wut_OSSetAlarm(WutAlarm*, OSTime, WutAlarmCallback) __asm__("OSSetAlarm");
BOOL wut_OSSetPeriodicAlarm(WutAlarm*, OSTime, OSTime, WutAlarmCallback) __asm__("OSSetPeriodicAlarm");
BOOL wut_OSCancelAlarm(WutAlarm*) __asm__("OSCancelAlarm");
void wut_OSSetAlarmUserData(WutAlarm*, void*) __asm__("OSSetAlarmUserData");
void* wut_OSGetAlarmUserData(WutAlarm*) __asm__("OSGetAlarmUserData");
OSTime wut_OSGetTime(void) __asm__("OSGetTime");
void* wut_OSGetCurrentThread(void) __asm__("OSGetCurrentThread");
BOOL wut_spin_acquire(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Acquire");
BOOL wut_spin_release(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Release");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
}

const unsigned kMaxAlarms = 32;
const unsigned kMaxRunners = 4;

struct AlarmRecord;

struct AlarmInstance {
    WutAlarm wut;                 // first: a callback's alarm is its instance
    AlarmRecord* record;
    uint64_t generation;          // immutable identity of this arming
    uint32_t callbacks;           // coreinit callbacks using this instance
    bool callback_started;
    bool periodic;
    bool retired;
    bool cleanup_claimed;
    bool disarm_in_progress;
};

struct AlarmRecord {
    AlarmInstance* instance;
    OSAlarm* game;
    OSAlarmHandler handler;
    OSTime due;                   // the arming's next due time (coreinit ticks)
    OSTime start, period;         // period 0 = one-shot
    void* runners[kMaxRunners];   // threads running this record's handler
    uint32_t in_flight;           // handlers running for this record
    uint32_t released;            // release sequence (0 = never used)
    uint64_t generation;          // next generation for this record
    bool armed;                   // an arming is live
    bool active;                  // owned by a game alarm
};

WutSpinLock s_lock;
AlarmRecord s_alarms[kMaxAlarms];
uint32_t s_release_seq;

struct Lock {
    Lock() { wut_spin_acquire(&s_lock); }
    ~Lock() { wut_spin_release(&s_lock); }
};

/* Caller holds s_lock. */
AlarmRecord* find_locked(OSAlarm* game)
{
    for (unsigned i = 0; i < kMaxAlarms; ++i)
        if (s_alarms[i].active && s_alarms[i].game == game)
            return &s_alarms[i];
    return nullptr;
}

/* Caller holds s_lock. Takes the free record released longest ago (never
 * used ones first) with no handler in flight. */
AlarmRecord* claim_locked(OSAlarm* game)
{
    AlarmRecord* best = nullptr;
    for (unsigned i = 0; i < kMaxAlarms; ++i) {
        AlarmRecord* r = &s_alarms[i];
        if (r->active || r->in_flight)
            continue;
        if (!best || r->released < best->released)
            best = r;
    }
    if (!best)
        return nullptr;
    best->instance = nullptr;
    best->game = game;
    best->armed = false;
    best->active = true;
    return best;
}

/* Caller holds s_lock: handlers of r running on the calling thread. */
uint32_t self_runs_locked(const AlarmRecord* r, void* self)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < kMaxRunners; ++i)
        if (r->runners[i] == self)
            ++n;
    return n;
}

void trampoline(WutAlarm* wut, void* /*context*/)
{
    AlarmInstance* instance = reinterpret_cast<AlarmInstance*>(wut);
    if (!instance || !instance->record)
        return;
    AlarmRecord* r = instance->record;
    if (r < s_alarms || r >= s_alarms + kMaxAlarms)
        return;
    void* self = wut_OSGetCurrentThread();
    void* data = wut_OSGetAlarmUserData(&instance->wut);
    const uint64_t generation = data ? *static_cast<uint64_t*>(data) : 0;
    OSAlarmHandler handler;
    OSAlarm* game;
    unsigned slot = kMaxRunners;
    bool run = false;
    bool destroy = false;
    {
        Lock lock;
        ++instance->callbacks;
        instance->callback_started = true;
        if (instance == r->instance && generation == r->generation &&
            r->active && r->armed && r->handler) {
            const OSTime now = wut_OSGetTime();
            if (now >= r->due) {
                if (r->period > 0)
                    r->due = r->start + ((now - r->start) / r->period + 1) * r->period;
                else
                    r->armed = false;     // a one-shot fires once
                handler = r->handler;
                game = r->game;
                ++r->in_flight;
                for (unsigned i = 0; i < kMaxRunners; ++i)
                    if (!r->runners[i]) {
                        r->runners[i] = self;
                        slot = i;
                        break;
                    }
                run = true;
            }
        }
    }
    if (run)
        handler(game, nullptr);
    {
        Lock lock;
        if (run) {
            if (slot < kMaxRunners)
                r->runners[slot] = nullptr;
            --r->in_flight;
        }
        --instance->callbacks;
        if (instance->retired && instance->callbacks == 0 &&
            !instance->cleanup_claimed && !instance->disarm_in_progress) {
            instance->cleanup_claimed = true;
            destroy = true;
        }
    }
    if (destroy)
        delete instance;
}

/* Disarm, stop the coreinit alarm, and wait for handlers still running on
 * other threads (one running on this thread is the caller itself). */
void disarm(AlarmRecord* r)
{
    void* self = wut_OSGetCurrentThread();
    AlarmInstance* instance;
    {
        Lock lock;
        r->armed = false;
        instance = r->instance;
        r->instance = nullptr;
        if (instance) {
            instance->retired = true;
            instance->disarm_in_progress = true;
        }
    }
    if (!instance)
        return;
    const BOOL cancelled = wut_OSCancelAlarm(&instance->wut);
    for (;;) {
        {
            Lock lock;
            if (r->in_flight <= self_runs_locked(r, self))
                break;
        }
        wut_OSYieldThread();
    }
    bool destroy = false;
    {
        Lock lock;
        /* A cancelled alarm cannot start a callback after this point. A
         * one-shot whose callback already ran is likewise finished even
         * though coreinit reports it was no longer queued. */
        destroy = instance->callbacks == 0 &&
                  (cancelled || (!instance->periodic && instance->callback_started));
        if (destroy)
            instance->cleanup_claimed = true;
        instance->disarm_in_progress = false;
    }
    if (destroy)
        delete instance;
}

/* Disarm the game alarm's record, if any, and return it to the free pool. */
void retire(OSAlarm* game)
{
    AlarmRecord* r;
    {
        Lock lock;
        r = find_locked(game);
    }
    if (!r)
        return;
    disarm(r);
    Lock lock;
    r->active = false;
    r->game = nullptr;
    r->handler = nullptr;
    r->released = ++s_release_seq;
}

/* Claim a fresh record for this arming; due/period are set before the
 * coreinit alarm is armed, from an earlier OSGetTime than coreinit's own, so
 * coreinit's fire time is never before due. */
AlarmRecord* arm_record(OSAlarm* alarm, OSAlarmHandler handler, OSTime first, OSTime start,
                        OSTime period)
{
    retire(alarm);
    AlarmInstance* instance = new (std::nothrow) AlarmInstance{};
    if (!instance) {
        port_log("[smg-alarm] alarm instance allocation failed");
        return nullptr;
    }
    Lock lock;
    AlarmRecord* r = claim_locked(alarm);
    if (!r) {
        port_log("[smg-alarm] alarm side table exhausted");
        delete instance;
        return nullptr;
    }
    instance->record = r;
    instance->generation = ++r->generation;
    instance->periodic = period > 0;
    r->instance = instance;
    const OSTime now = wut_OSGetTime();
    r->handler = handler;
    r->period = period;
    r->start = start;
    if (period > 0)
        r->due = start > now ? start : start + ((now - start) / period + 1) * period;
    else
        r->due = now + first;
    r->armed = true;
    wut_OSCreateAlarm(&instance->wut);
    wut_OSSetAlarmUserData(&instance->wut, &instance->generation);
    return r;
}

} // namespace

extern "C" {

void smg_wiiu_OSCreateAlarm(OSAlarm* alarm)
{
    if (!alarm)
        return;
    retire(alarm);
    std::memset(alarm, 0, sizeof(*alarm));
}

void smg_wiiu_OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    if (!alarm)
        return;
    AlarmRecord* r = arm_record(alarm, handler, tick, 0, 0);
    if (!r)
        return;
    alarm->handler = handler;
    wut_OSSetAlarm(&r->instance->wut, tick, trampoline);
}

void smg_wiiu_OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                                 OSAlarmHandler handler)
{
    if (!alarm)
        return;
    AlarmRecord* r = arm_record(alarm, handler, 0, start, period);
    if (!r)
        return;
    alarm->handler = handler;
    alarm->start = start;
    alarm->period = period;
    wut_OSSetPeriodicAlarm(&r->instance->wut, start, period, trampoline);
}

void smg_wiiu_OSCancelAlarm(OSAlarm* alarm)
{
    if (!alarm)
        return;
    /* MainLoopFramework creates a stack alarm every frame: the record goes
     * back to the (least recently released first) free pool. */
    retire(alarm);
    alarm->handler = nullptr;
}

} // extern "C"
