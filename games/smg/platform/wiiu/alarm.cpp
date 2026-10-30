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
 *    the same coreinit timebase when the alarm is armed. Native alarms are
 *    always one-shot: a game periodic alarm advances its due time and
 *    re-arms the same native alarm before its handler runs.
 *  - Every arming takes a free record (least recently released first).
 *    Instances have explicit record, native-alarm and callback references;
 *    callback-context disarm retires them without waiting, while external
 *    disarm still waits for handlers so its return remains a completion
 *    boundary. Records with handlers in flight are not reused. The native
 *    thread-side native arm/cancel calls happen outside s_lock. A pending
 *    native arm keeps its record reserved until the call completes, and an
 *    operation reference keeps its instance alive across each native call.
 */
#include "port/port.h"

#include <revolution/os.h>
#include <revolution/os/OSAlarm.h>

#include <cstdint>
#include <cstring>
#include <new>
#if defined(SMG_ALARM_HOST_TEST)
#include <atomic>
#endif

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
struct AlarmRecord;

struct AlarmInstance {
    WutAlarm wut;                 // first: a callback's alarm is its instance
    AlarmRecord* record;
    uint64_t generation;          // immutable identity of this arming
    uint32_t refs;                // record + native alarm + callback holders
    bool periodic;                // game alarm is periodic
    bool native_held;              // the currently armed native one-shot
};

struct AlarmRecord {
    AlarmInstance* instance;
    OSAlarm* game;
    OSAlarmHandler handler;
    OSTime due;                   // the arming's next due time (coreinit ticks)
    OSTime start, period;         // period 0 = one-shot
    uint32_t in_flight;           // handlers running for this record
    uint32_t released;            // release sequence (0 = never used)
    uint64_t generation;          // next generation for this record
    bool armed;                   // an arming is live
    bool active;                  // owned by a game alarm
    bool native_arm_pending;      // thread-side OSSetAlarm is outside s_lock
};

WutSpinLock s_lock;
AlarmRecord s_alarms[kMaxAlarms];
uint32_t s_release_seq;
void* s_handler_threads[kMaxAlarms];
uint32_t s_handler_depth[kMaxAlarms];
#if defined(SMG_ALARM_HOST_TEST)
std::atomic<int> s_live_instances{0};
#endif

void destroy_instance(AlarmInstance* instance)
{
#if defined(SMG_ALARM_HOST_TEST)
    --s_live_instances;
#endif
    delete instance;
}

struct Lock {
    Lock() { wut_spin_acquire(&s_lock); }
    ~Lock() { wut_spin_release(&s_lock); }
};

bool handler_thread_locked(void* thread)
{
    for (unsigned i = 0; i < kMaxAlarms; ++i)
        if (s_handler_threads[i] == thread && s_handler_depth[i] != 0)
            return true;
    return false;
}

void begin_handler()
{
    void* thread = wut_OSGetCurrentThread();
    Lock lock;
    for (unsigned i = 0; i < kMaxAlarms; ++i) {
        if (s_handler_threads[i] == thread) {
            ++s_handler_depth[i];
            return;
        }
    }
    for (unsigned i = 0; i < kMaxAlarms; ++i) {
        if (s_handler_depth[i] == 0) {
            s_handler_threads[i] = thread;
            s_handler_depth[i] = 1;
            return;
        }
    }
}

void end_handler()
{
    void* thread = wut_OSGetCurrentThread();
    Lock lock;
    for (unsigned i = 0; i < kMaxAlarms; ++i) {
        if (s_handler_threads[i] == thread && s_handler_depth[i] != 0) {
            if (--s_handler_depth[i] == 0)
                s_handler_threads[i] = nullptr;
            return;
        }
    }
}

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
        if (r->active || r->in_flight || r->native_arm_pending)
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

/* Caller holds s_lock. Return whether the final holder was released. */
bool release_locked(AlarmInstance* instance)
{
    return --instance->refs == 0;
}

/* Caller holds s_lock. Each native one-shot has one native hold. A callback
 * consumes that hold on entry; cancellation consumes it only when native
 * cancellation succeeds. */
bool release_native_locked(AlarmInstance* instance)
{
    if (!instance->native_held)
        return false;
    instance->native_held = false;
    return release_locked(instance);
}

/* Caller holds s_lock. Return the first start+n*period deadline after now;
 * unlike incrementing the previous due time, this cannot accumulate handler
 * or dispatch latency. */
OSTime next_period_due(const AlarmRecord* r, OSTime now)
{
    if (now < r->start)
        return r->start;
    return r->start + ((now - r->start) / r->period + 1) * r->period;
}

void trampoline(WutAlarm* wut, void* /*context*/)
{
    AlarmInstance* instance = reinterpret_cast<AlarmInstance*>(wut);
    if (!instance)
        return;
    OSAlarmHandler handler;
    OSAlarm* game;
    AlarmRecord* r;
    bool run = false;
    bool destroy = false;
    {
        Lock lock;
        r = instance->record;
        if (!r || r < s_alarms || r >= s_alarms + kMaxAlarms)
            return;
        ++instance->refs;             // callback holder; native holder keeps it alive
        bool native_destroy = release_native_locked(instance);
        if (native_destroy)
            destroy = true;
        void* data = wut_OSGetAlarmUserData(&instance->wut);
        const uint64_t generation = data ? *static_cast<uint64_t*>(data) : 0;
        if (instance == r->instance && generation == r->generation &&
            r->active && r->armed) {
            const OSTime now = wut_OSGetTime();
            const bool due = now >= r->due;
            if (r->period > 0) {
                if (due)
                    r->due = next_period_due(r, now);
                /* A game periodic alarm is represented by a succession of
                 * native one-shots. Keep this native hold across the re-arm,
                 * and do it before releasing s_lock or entering the game
                 * handler. */
                ++instance->refs;
                instance->native_held = true;
                if (!wut_OSSetAlarm(&instance->wut, r->due - now, trampoline)) {
                    instance->native_held = false;
                    if (release_locked(instance))
                        destroy = true;
                    r->armed = false;
                    r->instance = nullptr;
                    if (release_locked(instance))
                        destroy = true;
                }
            } else if (due) {
                r->armed = false;     // a one-shot fires once
            }
            if (due && r->handler) {
                handler = r->handler;
                game = r->game;
                ++r->in_flight;
                run = true;
            }
        }
    }
    if (run) {
        begin_handler();
        handler(game, nullptr);
        end_handler();
    }
    {
        Lock lock;
        if (run)
            --r->in_flight;

        /* A fired one-shot no longer needs the record's instance hold. Keep
         * the record itself active so a later cancel/re-arm can retire it. */
        if (!instance->periodic && r->instance == instance && !r->armed) {
            r->instance = nullptr;
            if (release_locked(instance))
                destroy = true;
        }

        if (release_locked(instance))
            destroy = true;
    }
    if (destroy)
        destroy_instance(instance);
}

/* Disarm and stop the coreinit alarm. Callback-context callers do not wait;
 * external callers wait for handlers after the native transition. The native
 * hold keeps a dequeued callback's instance alive until it enters the
 * trampoline; its callback hold then keeps it alive through the handler. */
void disarm(AlarmRecord* r)
{
    bool defer_wait;
    AlarmInstance* instance;
    bool destroy = false;
    {
        Lock lock;
        defer_wait = handler_thread_locked(wut_OSGetCurrentThread());
        r->armed = false;
        instance = r->instance;
        r->instance = nullptr;
        if (instance) {
            ++instance->refs;           // hold across the thread-side cancel
            if (release_locked(instance))
                destroy = true;       // drop the record holder
        }
    }
    if (instance) {
        /* A thread-side arm may still be between publication and OSSetAlarm.
         * Let that native transition finish before cancelling, while the
         * operation reference keeps the instance alive. */
        for (;;) {
            {
                Lock lock;
                if (!r->native_arm_pending)
                    break;
            }
            wut_OSYieldThread();
        }

        const BOOL cancelled = wut_OSCancelAlarm(&instance->wut);
        {
            Lock lock;
            if (cancelled && release_native_locked(instance))
                destroy = true;
            if (release_locked(instance))
                destroy = true;       // drop the cancel operation holder
        }
    }
    if (instance && !defer_wait) {
        for (;;) {
            {
                Lock lock;
                if (r->in_flight == 0)
                    break;
            }
            wut_OSYieldThread();
        }
    }
    if (destroy)
        destroy_instance(instance);
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
#if defined(SMG_ALARM_HOST_TEST)
    ++s_live_instances;
#endif
    /* Create and identify the native alarm before publishing the instance.
     * The generation field is initialized before OSSetAlarm can dispatch it. */
    wut_OSCreateAlarm(&instance->wut);
    wut_OSSetAlarmUserData(&instance->wut, &instance->generation);

    AlarmRecord* r;
    OSTime delay;
    {
        Lock lock;
        r = claim_locked(alarm);
        if (!r) {
            port_log("[smg-alarm] alarm side table exhausted");
            destroy_instance(instance);
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
        r->native_arm_pending = true;
        alarm->handler = handler;
        alarm->start = start;
        alarm->period = period;
        instance->refs = 3;            // record + native + arm-call holders
        instance->native_held = true;
        delay = period > 0 ? r->due - now : first;
    }

    const BOOL set = wut_OSSetAlarm(&instance->wut, delay, trampoline);
    bool destroy = false;
    {
        Lock lock;
        r->native_arm_pending = false;
        if (!set && r->instance == instance && r->armed) {
            r->armed = false;
            r->instance = nullptr;
            if (release_locked(instance))
                destroy = true;       // drop the record holder
            if (release_native_locked(instance))
                destroy = true;
        }
        if (release_locked(instance))
            destroy = true;            // drop the arm-call holder
    }
    if (destroy)
        destroy_instance(instance);
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
    (void)r;
}

void smg_wiiu_OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                                 OSAlarmHandler handler)
{
    if (!alarm)
        return;
    AlarmRecord* r = arm_record(alarm, handler, 0, start, period);
    if (!r)
        return;
    (void)r;
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

#if defined(SMG_ALARM_HOST_TEST)
extern "C" int smg_wiiu_alarm_live_instances()
{
    return s_live_instances.load();
}
#endif
