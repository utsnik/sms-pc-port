/* PC-sampling profiler for hardware runs.
 *
 * A periodic coreinit alarm armed from the main thread fires on its core (the
 * game threads share it) and receives the interrupted context; its PC goes
 * into a fixed hash table. Every ~10 s the frame boundary swaps tables,
 * resolves each PC to its function with OSGetSymbolName and logs the top
 * functions. The alarm callback takes no locks and allocates nothing.
 *
 * The game's SDK has its own OSAlarm, so coreinit is bound by symbol name. */
#include "runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

typedef int64_t WutTime;
typedef void (*WutAlarmCallback)(void* alarm, void* context);

extern "C" {
void wut_OSCreateAlarm(void*) __asm__("OSCreateAlarm");
int wut_OSSetPeriodicAlarm(void*, WutTime, WutTime, WutAlarmCallback) __asm__("OSSetPeriodicAlarm");
int wut_OSCancelAlarm(void*) __asm__("OSCancelAlarm");
WutTime wut_OSGetTime(void) __asm__("OSGetTime");
uint32_t wut_OSGetSymbolName(uint32_t, char*, uint32_t) __asm__("OSGetSymbolName");
void* wut_OSGetCurrentThread(void) __asm__("OSGetCurrentThread");
}

constexpr WutTime kTimerHz = 248625000 / 4;
// Not a divisor of the 10 ms AX tick, so the samples do not phase-lock to it.
constexpr WutTime kPeriod = kTimerHz / 1013;
constexpr WutTime kReportEvery = kTimerHz * 10;
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kSrr0Offset = 0x98;

struct Table {
	uint32_t pc[kSlots];
	uint32_t count[kSlots];
	uint32_t samples;
	uint32_t dropped;
	uint32_t main;  // samples that interrupted the main (game) thread
};

alignas(64) uint8_t s_alarm[0x80];
Table s_tables[2];
volatile uint32_t s_active;
void* s_mainThread;  // an OSThread starts with its OSContext
volatile uint32_t s_frameTicks;  // frame boundaries seen, for the hang check
bool s_running;
WutTime s_lastReport;

void on_alarm(void*, void* context)
{
	const uint32_t pc = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(context) + kSrr0Offset);
	Table& t = s_tables[s_active & 1];
	++t.samples;
	if (context == s_mainThread) ++t.main;
	uint32_t slot = (pc >> 2) * 2654435761u % kSlots;
	for (uint32_t probe = 0; probe < 32; ++probe, slot = (slot + 1) % kSlots) {
		if (t.pc[slot] == pc) { ++t.count[slot]; return; }
		if (!t.pc[slot]) { t.pc[slot] = pc; t.count[slot] = 1; return; }
	}
	++t.dropped;
}

struct Func {
	char name[64];
	uint32_t count;
};

void report(Table& t)
{
	static Func funcs[512];
	uint32_t nfuncs = 0, other = 0;
	for (uint32_t i = 0; i < kSlots; ++i) {
		if (!t.pc[i]) continue;
		char name[64] = {};
		wut_OSGetSymbolName(t.pc[i], name, sizeof(name));
		uint32_t f = 0;
		while (f < nfuncs && std::strcmp(funcs[f].name, name) != 0) ++f;
		if (f == nfuncs) {
			if (nfuncs == 512) { other += t.count[i]; continue; }
			std::memcpy(funcs[f].name, name, sizeof(name));
			funcs[f].count = 0;
			++nfuncs;
		}
		funcs[f].count += t.count[i];
	}
	port_log("[prof] samples=%u dropped=%u functions=%u main-thread=%u\n", (unsigned)t.samples, (unsigned)t.dropped, (unsigned)nfuncs, (unsigned)t.main);
	for (int rank = 0; rank < 25; ++rank) {
		uint32_t best = nfuncs;
		for (uint32_t f = 0; f < nfuncs; ++f)
			if (funcs[f].count && (best == nfuncs || funcs[f].count > funcs[best].count)) best = f;
		if (best == nfuncs) break;
		port_log("[prof] %2d %5.1f%% %5u %s\n", rank, t.samples ? 100.0f * funcs[best].count / t.samples : 0.0f,
		         (unsigned)funcs[best].count, funcs[best].name);
		funcs[best].count = 0;
	}
	(void)other;
	std::memset(&t, 0, sizeof(t));
}

} // namespace

/* Called from the frame boundary on the main thread: starts the sampler the
 * first time and reports every ~10 s. */
extern "C" void sms_wiiu_profiler_tick(void)
{
	const WutTime now = wut_OSGetTime();
	++s_frameTicks;
	if (!s_running) {
		std::memset(s_tables, 0, sizeof(s_tables));
		s_mainThread = wut_OSGetCurrentThread();
		wut_OSCreateAlarm(s_alarm);
		s_running = wut_OSSetPeriodicAlarm(s_alarm, now + kPeriod, kPeriod, on_alarm) != 0;
		s_lastReport = now;
		port_log("[prof] sampler %s (period %u ticks)\n", s_running ? "started" : "FAILED", (unsigned)kPeriod);
		return;
	}
	if (now - s_lastReport < kReportEvery) return;
	s_lastReport = now;
	const uint32_t done = s_active & 1;
	s_active = done ^ 1;
	report(s_tables[done]);
}

/* Hang report, from another thread (the audio pacer's 10 s stats): when the
 * frame boundary has not run since the last check, log where the main thread
 * is parked, from its saved context (an OSThread starts with its OSContext:
 * gpr1 at 0x0C, lr at 0x8C, srr0 at 0x98) and its stack's back chain. A
 * thread spinning on its core shows its last switch-out instead. */
extern "C" void sms_wiiu_hang_check(void)
{
	static uint32_t s_lastTicks, s_reported;
	const uint32_t ticks = s_frameTicks;
	if (ticks != s_lastTicks || !s_mainThread) {
		s_lastTicks = ticks;
		s_reported = 0;
		return;
	}
	if (s_reported >= 3) return;
	++s_reported;
	const uint8_t* ctx = static_cast<const uint8_t*>(s_mainThread);
	auto word = [](uint32_t addr) { return *reinterpret_cast<const volatile uint32_t*>(addr); };
	auto name = [](uint32_t pc, char* out, uint32_t size) {
		out[0] = 0;
		wut_OSGetSymbolName(pc, out, size);
	};
	char sym[64];
	const uint32_t srr0 = *reinterpret_cast<const uint32_t*>(ctx + 0x98);
	const uint32_t lr = *reinterpret_cast<const uint32_t*>(ctx + 0x8C);
	name(srr0, sym, sizeof(sym));
	port_log("[hang] no frame for 10 s: main pc=%08x %s\n", (unsigned)srr0, sym);
	name(lr, sym, sizeof(sym));
	port_log("[hang]   lr=%08x %s\n", (unsigned)lr, sym);
	uint32_t sp = *reinterpret_cast<const uint32_t*>(ctx + 0x0C);
	for (int depth = 0; depth < 14; ++depth) {
		if (sp < 0x10000000u || sp >= 0x50000000u || (sp & 3)) break;
		const uint32_t next = word(sp);
		if (next <= sp || next >= 0x50000000u) break;
		const uint32_t ret = word(next + 4);
		name(ret, sym, sizeof(sym));
		port_log("[hang]   #%d %08x %s\n", depth, (unsigned)ret, sym);
		sp = next;
	}
}

/* Every exit path cancels the alarm, or it fires into an unmapped process. */
extern "C" void sms_wiiu_profiler_stop(void)
{
	if (!s_running) return;
	wut_OSCancelAlarm(s_alarm);
	s_running = false;
}
