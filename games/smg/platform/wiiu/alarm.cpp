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
 *  - A coreinit callback is only a wake-up. Its record is the coreinit alarm
 *    it was called with (records are static and never change identity), and
 *    under the lock it runs the game handler only if that record is armed
 *    and its arming is due: OSGetTime() >= due, where due is computed from
 *    the same coreinit timebase when the alarm is armed. A one-shot then
 *    disarms; a periodic alarm advances due to its next period. So a late
 *    callback left over from an earlier arming, however old and whatever
 *    has been reused since, can neither run a handler early nor twice: no
 *    ticket or generation has to survive a wraparound.
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
OSTime wut_OSGetTime(void) __asm__("OSGetTime");
void* wut_OSGetCurrentThread(void) __asm__("OSGetCurrentThread");
BOOL wut_spin_acquire(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Acquire");
BOOL wut_spin_release(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Release");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
}

const unsigned kMaxAlarms = 32;
const unsigned kMaxRunners = 4;

struct AlarmRecord {
    WutAlarm wut;                 // first: a callback's alarm is its record
    OSAlarm* game;
    OSAlarmHandler handler;
    OSTime due;                   // the arming's next due time (coreinit ticks)
    OSTime start, period;         // period 0 = one-shot
    void* runners[kMaxRunners];   // threads running this record's handler
    uint32_t in_flight;           // handlers running for this record
    uint32_t released;            // release sequence (0 = never used)
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
    best->game = game;
    best->armed = false;
    best->active = true;
    wut_OSCreateAlarm(&best->wut);
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
    AlarmRecord* r = reinterpret_cast<AlarmRecord*>(wut);
    if (r < s_alarms || r >= s_alarms + kMaxAlarms)
        return;
    void* self = wut_OSGetCurrentThread();
    OSAlarmHandler handler;
    OSAlarm* game;
    unsigned slot = kMaxRunners;
    {
        Lock lock;
        if (!r->active || !r->armed || !r->handler)
            return;
        const OSTime now = wut_OSGetTime();
        if (now < r->due)
            return;                       // not this arming's fire: stale
        if (r->period > 0)
            r->due = r->start + ((now - r->start) / r->period + 1) * r->period;
        else
            r->armed = false;             // a one-shot fires once
        handler = r->handler;
        game = r->game;
        ++r->in_flight;
        for (unsigned i = 0; i < kMaxRunners; ++i)
            if (!r->runners[i]) {
                r->runners[i] = self;
                slot = i;
                break;
            }
    }
    handler(game, nullptr);
    Lock lock;
    if (slot < kMaxRunners)
        r->runners[slot] = nullptr;
    --r->in_flight;
}

/* Disarm, stop the coreinit alarm, and wait for handlers still running on
 * other threads (one running on this thread is the caller itself). */
void disarm(AlarmRecord* r)
{
    void* self = wut_OSGetCurrentThread();
    {
        Lock lock;
        r->armed = false;
    }
    wut_OSCancelAlarm(&r->wut);
    for (;;) {
        {
            Lock lock;
            if (r->in_flight <= self_runs_locked(r, self))
                return;
        }
        wut_OSYieldThread();
    }
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
    Lock lock;
    AlarmRecord* r = claim_locked(alarm);
    if (!r) {
        port_log("[smg-alarm] alarm side table exhausted");
        return nullptr;
    }
    const OSTime now = wut_OSGetTime();
    r->handler = handler;
    r->period = period;
    r->start = start;
    if (period > 0)
        r->due = start > now ? start : start + ((now - start) / period + 1) * period;
    else
        r->due = now + first;
    r->armed = true;
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
    wut_OSSetAlarm(&r->wut, tick, trampoline);
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
    wut_OSSetPeriodicAlarm(&r->wut, start, period, trampoline);
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
