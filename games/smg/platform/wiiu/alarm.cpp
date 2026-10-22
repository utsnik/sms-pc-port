/* Galaxy OSAlarm adapter (Wii SDK alarm API on Wii U coreinit).
 *
 * Galaxy's OSAlarm (revolution/os/OSAlarm.h) is the Wii's 0x30-byte struct;
 * the Wii SDK names resolve to coreinit's imports, whose OSAlarm is 0x58 bytes.
 * Unadapted, OSCreateAlarm/OSSetPeriodicAlarm on SpkSpeakerCtrl's static
 * sSpeakerAlarm wrote 0x28 bytes past it, over FileRipper's sReadBuffer and
 * sReadBufferEnd, and the next Yaz0 load stored through a null read buffer.
 *
 * Each game alarm gets its own coreinit alarm from a side table; the coreinit
 * callback calls the game handler with the game alarm. Ticks pass through
 * unchanged (Galaxy's OSGetTime is coreinit's). There is no Wii OSContext for
 * the handler: it receives nullptr (Galaxy's two handlers ignore it).
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

extern "C" {
void wut_OSCreateAlarm(WutAlarm*) __asm__("OSCreateAlarm");
BOOL wut_OSSetAlarm(WutAlarm*, OSTime, WutAlarmCallback) __asm__("OSSetAlarm");
BOOL wut_OSSetPeriodicAlarm(WutAlarm*, OSTime, OSTime, WutAlarmCallback) __asm__("OSSetPeriodicAlarm");
BOOL wut_OSCancelAlarm(WutAlarm*) __asm__("OSCancelAlarm");
void wut_OSSetAlarmUserData(WutAlarm*, void*) __asm__("OSSetAlarmUserData");
void* wut_OSGetAlarmUserData(WutAlarm*) __asm__("OSGetAlarmUserData");
}

const unsigned kMaxAlarms = 32;

struct AlarmRecord {
    WutAlarm wut;
    OSAlarm* game;
    volatile OSAlarmHandler handler;
    bool active;
};

AlarmRecord s_alarms[kMaxAlarms];

AlarmRecord* record_for(OSAlarm* game, bool create)
{
    for (unsigned i = 0; i < kMaxAlarms; ++i)
        if (s_alarms[i].active && s_alarms[i].game == game)
            return &s_alarms[i];
    if (!create)
        return nullptr;
    for (unsigned i = 0; i < kMaxAlarms; ++i) {
        if (s_alarms[i].active)
            continue;
        AlarmRecord* r = &s_alarms[i];
        r->game = game;
        r->handler = nullptr;
        r->active = true;
        wut_OSCreateAlarm(&r->wut);
        wut_OSSetAlarmUserData(&r->wut, r);
        return r;
    }
    port_log("[smg-alarm] alarm side table exhausted");
    return nullptr;
}

void trampoline(WutAlarm* wut, void*)
{
    AlarmRecord* r = static_cast<AlarmRecord*>(wut_OSGetAlarmUserData(wut));
    const OSAlarmHandler handler = r ? r->handler : nullptr;
    if (handler)
        handler(r->game, nullptr);
}

} // namespace

extern "C" {

void smg_wiiu_OSCreateAlarm(OSAlarm* alarm)
{
    if (!alarm)
        return;
    AlarmRecord* r = record_for(alarm, false);
    if (r)
        wut_OSCancelAlarm(&r->wut);
    std::memset(alarm, 0, sizeof(*alarm));
    record_for(alarm, true);
}

void smg_wiiu_OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    AlarmRecord* r = record_for(alarm, true);
    if (!r)
        return;
    alarm->handler = handler;
    r->handler = handler;
    wut_OSSetAlarm(&r->wut, tick, trampoline);
}

void smg_wiiu_OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                                 OSAlarmHandler handler)
{
    AlarmRecord* r = record_for(alarm, true);
    if (!r)
        return;
    alarm->handler = handler;
    alarm->start = start;
    alarm->period = period;
    r->handler = handler;
    wut_OSSetPeriodicAlarm(&r->wut, start, period, trampoline);
}

void smg_wiiu_OSCancelAlarm(OSAlarm* alarm)
{
    AlarmRecord* r = record_for(alarm, false);
    if (!r)
        return;
    wut_OSCancelAlarm(&r->wut);
    r->handler = nullptr;
    /* MainLoopFramework creates a stack alarm every frame: free the slot. */
    r->active = false;
    if (alarm)
        alarm->handler = nullptr;
}

} // extern "C"
