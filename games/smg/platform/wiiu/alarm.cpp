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
 * create, arm and cancel, all under one uninterruptible spinlock.
 *  - Every arming takes a fresh record and a fresh ticket (the coreinit
 *    alarm's user data). A callback runs the handler only if its ticket is
 *    still its record's current one, and pins the record (in-flight count)
 *    while the handler runs.
 *  - Re-arming or cancelling retires the record: its ticket is cleared, the
 *    coreinit alarm cancelled, in-flight handlers are waited for, and the
 *    record joins the free pool stamped with a release sequence.
 *  - A callback coreinit has already dequeued but that has not yet read its
 *    user data is beyond cancel's reach. Free records are therefore reused
 *    least recently released first: with kMaxAlarms records and two or three
 *    live alarms, a record is quarantined for ~30 releases (about half a
 *    second of MainLoopFramework's per-frame alarm) before its coreinit alarm
 *    is re-created, so such a callback finds no current ticket and returns.
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
void wut_OSSetAlarmUserData(WutAlarm*, void*) __asm__("OSSetAlarmUserData");
void* wut_OSGetAlarmUserData(WutAlarm*) __asm__("OSGetAlarmUserData");
BOOL wut_spin_acquire(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Acquire");
BOOL wut_spin_release(WutSpinLock*) __asm__("OSUninterruptibleSpinLock_Release");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
}

const unsigned kMaxAlarms = 32;
const unsigned kMaxTickets = 256;

struct AlarmRecord;

struct Ticket {
    AlarmRecord* record;
    OSAlarm* game;
    OSAlarmHandler handler;
};

struct AlarmRecord {
    WutAlarm wut;
    OSAlarm* game;
    Ticket* current;      // the live arming; nullptr when unarmed
    uint32_t in_flight;   // handlers running for this record
    uint32_t released;    // release sequence (0 = never used)
    bool active;          // owned by a game alarm
};

WutSpinLock s_lock;
AlarmRecord s_alarms[kMaxAlarms];
Ticket s_tickets[kMaxTickets];
unsigned s_next_ticket;
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
    best->current = nullptr;
    best->active = true;
    wut_OSCreateAlarm(&best->wut);
    return best;
}

/* Caller holds s_lock. Tickets are a ring: one is reused only after
 * kMaxTickets later armings, and a stale callback still finds it is not its
 * record's current ticket. */
Ticket* new_ticket_locked(AlarmRecord* r, OSAlarm* game, OSAlarmHandler handler)
{
    Ticket* t = &s_tickets[s_next_ticket++ % kMaxTickets];
    t->record = r;
    t->game = game;
    t->handler = handler;
    r->current = t;
    return t;
}

void trampoline(WutAlarm*, void* /*context*/);

void dispatch(Ticket* t)
{
    OSAlarmHandler handler = nullptr;
    OSAlarm* game = nullptr;
    AlarmRecord* r = nullptr;
    {
        Lock lock;
        if (!t || !t->record || t->record->current != t || !t->record->active)
            return;
        r = t->record;
        handler = t->handler;
        game = t->game;
        ++r->in_flight;
    }
    if (handler)
        handler(game, nullptr);
    Lock lock;
    --r->in_flight;
}

void trampoline(WutAlarm* wut, void*)
{
    dispatch(static_cast<Ticket*>(wut_OSGetAlarmUserData(wut)));
}

/* Retire the record's ticket, stop the coreinit alarm, and wait for handlers
 * already running. Not for use from inside the alarm's own handler. */
void disarm(AlarmRecord* r)
{
    {
        Lock lock;
        r->current = nullptr;
    }
    wut_OSCancelAlarm(&r->wut);
    for (;;) {
        {
            Lock lock;
            if (!r->in_flight)
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
    r->released = ++s_release_seq;
}

AlarmRecord* arm_record(OSAlarm* alarm, OSAlarmHandler handler, Ticket** ticket)
{
    retire(alarm);
    Lock lock;
    AlarmRecord* r = claim_locked(alarm);
    if (!r) {
        port_log("[smg-alarm] alarm side table exhausted");
        return nullptr;
    }
    *ticket = new_ticket_locked(r, alarm, handler);
    wut_OSSetAlarmUserData(&r->wut, *ticket);
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
    Ticket* ticket = nullptr;
    AlarmRecord* r = arm_record(alarm, handler, &ticket);
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
    Ticket* ticket = nullptr;
    AlarmRecord* r = arm_record(alarm, handler, &ticket);
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
