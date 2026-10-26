/* PC-sampling profiler for hardware runs.
 *
 * A periodic coreinit alarm armed from the main thread fires on its core (the
 * game threads share it) and receives the interrupted context; its PC goes
 * into a fixed hash table. Every ~10 s the frame boundary swaps tables,
 * resolves each PC to its function with OSGetSymbolName and logs the top
 * functions. The alarm callback takes no locks and allocates nothing.
 *
 * A leaf (memset, OSRestoreInterrupts, ...) says little by itself, and the
 * alarm is held off while interrupts are disabled, so samples pile up in
 * OSRestoreInterrupts. Once a report has seen which PCs resolve to a leaf,
 * later samples at those PCs are also counted by caller (LR) and the
 * caller's caller (from the stack back chain). Samples are also counted by
 * the interrupted thread, since the game's other threads share the core.
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
constexpr uint32_t kLrOffset = 0x8C;
constexpr uint32_t kSpOffset = 0x0C;  // gpr1
constexpr uint32_t kLeafSlots = 1024;
constexpr uint32_t kCallSlots = 1024;
constexpr uint32_t kThreads = 16;
constexpr uint32_t kIncSlots = 4096;
constexpr uint32_t kStackStartOffset = 0x394;  // OSThread::stackStart (the top)
constexpr uint32_t kStackEndOffset = 0x398;    // OSThread::stackEnd (the bottom)

struct Table {
	uint32_t pc[kSlots];
	uint32_t count[kSlots];
	uint8_t tid[kSlots];  // index into thread[] (kThreads = not tracked)
	// Leaf samples by call path: the leaf PC, its caller and that caller's caller.
	uint32_t callPc[kCallSlots], callLr[kCallSlots], callUp[kCallSlots], callCount[kCallSlots];
	void* thread[kThreads];
	uint32_t threadCount[kThreads];
	uint32_t samples;
	uint32_t dropped;
	uint32_t callDropped;
	uint32_t main;  // samples that interrupted the main (game) thread
	// Main-thread samples by every return address on the stack: what each
	// function and its callees cost together (inclusive).
	uint32_t incPc[kIncSlots], incCount[kIncSlots];
	uint32_t incDropped;
};

alignas(64) uint8_t s_alarm[0x80];
Table s_tables[2];
// SD marker "workerprof": a second sampler armed from the GX2 producer worker,
// so it fires on that thread's core (0); reported with the main thread's.
alignas(64) uint8_t s_walarm[0x80];
Table s_wtables[2];
// PCs known to be inside a leaf function, learned by report(); 0 = empty.
uint32_t s_leafPc[kLeafSlots];

inline uint32_t slot_of(uint32_t key, uint32_t slots) { return (key >> 2) * 2654435761u % slots; }

bool is_leaf_pc(uint32_t pc)
{
	uint32_t slot = slot_of(pc, kLeafSlots);
	for (uint32_t probe = 0; probe < 16; ++probe, slot = (slot + 1) % kLeafSlots) {
		if (s_leafPc[slot] == pc) return true;
		if (!s_leafPc[slot]) return false;
	}
	return false;
}

void learn_leaf_pc(uint32_t pc)
{
	uint32_t slot = slot_of(pc, kLeafSlots);
	for (uint32_t probe = 0; probe < 16; ++probe, slot = (slot + 1) % kLeafSlots) {
		if (s_leafPc[slot] == pc) return;
		if (!s_leafPc[slot]) { s_leafPc[slot] = pc; return; }
	}
}

inline bool stack_addr(uint32_t a) { return a >= 0x10000000u && a < 0x50000000u && !(a & 3); }
// OSGetSymbolName dereferences what it is given: only code addresses (system
// RPLs and the game's text) may be named. A stray stack word faulted it.
inline bool code_addr(uint32_t a) { return a >= 0x01000000u && a < 0x10000000u && !(a & 3); }

inline bool code_addr_(uint32_t a) { return a >= 0x01000000u && a < 0x10000000u && !(a & 3); }

void count_inclusive(Table& t, const uint8_t* ctx)
{
	const uint32_t top = *reinterpret_cast<const uint32_t*>(ctx + kStackStartOffset);
	const uint32_t bottom = *reinterpret_cast<const uint32_t*>(ctx + kStackEndOffset);
	uint32_t sp = *reinterpret_cast<const uint32_t*>(ctx + kSpOffset);
	for (int depth = 0; depth < 32; ++depth) {
		if (sp < bottom || sp + 8 > top || (sp & 3)) return;
		const uint32_t next = *reinterpret_cast<const volatile uint32_t*>(sp);
		if (next <= sp || next + 8 > top) return;
		const uint32_t ret = *reinterpret_cast<const volatile uint32_t*>(next + 4);
		sp = next;
		if (!code_addr_(ret)) continue;
		uint32_t slot = (ret >> 2) * 2654435761u % kIncSlots;
		for (uint32_t probe = 0;; ++probe, slot = (slot + 1) % kIncSlots) {
			if (probe == 32) { ++t.incDropped; break; }
			if (t.incPc[slot] == ret) { ++t.incCount[slot]; break; }
			if (!t.incPc[slot]) { t.incPc[slot] = ret; t.incCount[slot] = 1; break; }
		}
	}
}

void count_call(Table& t, const uint8_t* ctx, uint32_t pc)
{
	const uint32_t lr = *reinterpret_cast<const uint32_t*>(ctx + kLrOffset);
	// A leaf builds no frame, so the stack pointer is still the caller's
	// frame; its back-chain word leads to the frame holding the caller's
	// own return address.
	uint32_t up = 0;
	const uint32_t sp = *reinterpret_cast<const uint32_t*>(ctx + kSpOffset);
	if (stack_addr(sp)) {
		const uint32_t next = *reinterpret_cast<const volatile uint32_t*>(sp);
		if (stack_addr(next) && next > sp && next - sp < 0x10000u)
			up = *reinterpret_cast<const volatile uint32_t*>(next + 4);
	}
	if (!code_addr(up)) up = 0;
	uint32_t slot = slot_of(lr ^ (up * 31u), kCallSlots);
	for (uint32_t probe = 0; probe < 32; ++probe, slot = (slot + 1) % kCallSlots) {
		if (t.callCount[slot] && t.callLr[slot] == lr && t.callUp[slot] == up) { ++t.callCount[slot]; return; }
		if (!t.callCount[slot]) {
			t.callPc[slot] = pc; t.callLr[slot] = lr; t.callUp[slot] = up; t.callCount[slot] = 1;
			return;
		}
	}
	++t.callDropped;
}
volatile uint32_t s_active;
void* s_mainThread;  // an OSThread starts with its OSContext
volatile uint32_t s_frameTicks;  // frame boundaries seen, for the hang check
bool s_running;
WutTime s_lastReport;

void* s_workerThread;
volatile uint32_t s_wactive;
bool s_wrunning;

void sample(Table& t, void* mainThread, void* context)
{
	const uint32_t pc = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(context) + kSrr0Offset);
	++t.samples;
	if (context == mainThread) {
		++t.main;
		count_inclusive(t, static_cast<const uint8_t*>(context));
	}
	uint32_t tid = 0;
	for (; tid < kThreads; ++tid) {
		if (t.thread[tid] == context) { ++t.threadCount[tid]; break; }
		if (!t.thread[tid]) { t.thread[tid] = context; t.threadCount[tid] = 1; break; }
	}
	if (is_leaf_pc(pc)) count_call(t, static_cast<const uint8_t*>(context), pc);
	uint32_t slot = slot_of(pc ^ (tid << 28), kSlots);
	for (uint32_t probe = 0; probe < 32; ++probe, slot = (slot + 1) % kSlots) {
		if (t.pc[slot] == pc && t.tid[slot] == tid) { ++t.count[slot]; return; }
		if (!t.pc[slot]) { t.pc[slot] = pc; t.tid[slot] = (uint8_t)tid; t.count[slot] = 1; return; }
	}
	++t.dropped;
}

void on_alarm(void*, void* context) { sample(s_tables[s_active & 1], s_mainThread, context); }
void on_walarm(void*, void* context) { sample(s_wtables[s_wactive & 1], s_workerThread, context); }

// Leaves whose samples are worth charging to their callers.
bool leaf_name(const char* name)
{
	const char* bar = std::strchr(name, '|');
	const char* sym = bar ? bar + 1 : name;
	static const char* const kLeaves[] = {"OSRestoreInterrupts", "memset", "memcpy", "memcmp", "memmove",
	                                      "DCFlushRange", "DCInvalidateRange", "DCStoreRange"};
	for (const char* leaf : kLeaves)
		if (std::strcmp(sym, leaf) == 0) return true;
	return false;
}

// "module|symbol" -> "symbol", so three names fit on one log line.
const char* short_name(const char* name)
{
	const char* bar = std::strchr(name, '|');
	return bar ? bar + 1 : name;
}

// Only when the sampled context really is a live OSThread.
bool is_thread(void* context)
{
	const uint8_t* p = static_cast<const uint8_t*>(context);
	if (!p || !stack_addr(reinterpret_cast<uint32_t>(p))) return false;
	return *reinterpret_cast<const uint64_t*>(p) == 0x4F53436F6E747874ull &&  // OS_CONTEXT_TAG
	       *reinterpret_cast<const uint32_t*>(p + 0x320) == 0x74487244u;       // OS_THREAD_TAG
}

const char* thread_name(void* context)
{
	if (!is_thread(context)) return nullptr;
	const uint8_t* p = static_cast<const uint8_t*>(context);
	const char* name = *reinterpret_cast<const char* const*>(p + 0x5C0);
	const uint32_t at = reinterpret_cast<uint32_t>(name);
	return at >= 0x01000000u && at < 0x50000000u ? name : nullptr;
}

struct Func {
	char name[64];
	uint32_t count;
};

void report(Table& t, const char* tag, void* mainThread)
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
		if (leaf_name(name)) learn_leaf_pc(t.pc[i]);
	}
	/* No '%' in these lines' output: the console log handler formats the
	 * finished line again, so "0.6% safe|..." became a %s with a junk
	 * pointer and faulted in OSConsoleWrite. */
	port_log("%s samples=%u dropped=%u functions=%u main-thread=%u call-dropped=%u\n", tag, (unsigned)t.samples,
	         (unsigned)t.dropped, (unsigned)nfuncs, (unsigned)t.main, (unsigned)t.callDropped);
	for (uint32_t i = 0; i < kThreads && t.thread[i]; ++i) {
		const char* tn = thread_name(t.thread[i]);
		char label[40];
		std::snprintf(label, sizeof(label), "%.31s", tn ? tn : "?");
		port_log("%s thr %08x %5.1f pct %s%s\n", tag, (unsigned)reinterpret_cast<uint32_t>(t.thread[i]),
		         t.samples ? 100.0f * t.threadCount[i] / t.samples : 0.0f, label,
		         t.thread[i] == mainThread ? " (main)" : "");
	}
	// Each other thread with a real share: its own top functions.
	for (uint32_t th = 0; th < kThreads && t.thread[th]; ++th) {
		if (t.thread[th] == mainThread || t.threadCount[th] * 50 < t.samples) continue;
		static Func tf[64];
		uint32_t ntf = 0;
		for (uint32_t i = 0; i < kSlots; ++i) {
			if (!t.pc[i] || t.tid[i] != th) continue;
			char name[64] = {};
			wut_OSGetSymbolName(t.pc[i], name, sizeof(name));
			uint32_t f = 0;
			while (f < ntf && std::strcmp(tf[f].name, name) != 0) ++f;
			if (f == ntf) {
				if (ntf == 64) continue;
				std::memcpy(tf[f].name, name, sizeof(name));
				tf[f].count = 0;
				++ntf;
			}
			tf[f].count += t.count[i];
		}
		for (int rank = 0; rank < 6; ++rank) {
			uint32_t best = ntf;
			for (uint32_t f = 0; f < ntf; ++f)
				if (tf[f].count && (best == ntf || tf[f].count > tf[best].count)) best = f;
			if (best == ntf) break;
			port_log("%s thr %08x %5.1f pct %s\n", tag, (unsigned)reinterpret_cast<uint32_t>(t.thread[th]),
			         100.0f * tf[best].count / t.samples, tf[best].name);
			tf[best].count = 0;
		}
	}
	// Leaf samples by call path, largest first.
	for (int rank = 0; rank < 20; ++rank) {
		uint32_t best = kCallSlots;
		for (uint32_t i = 0; i < kCallSlots; ++i)
			if (t.callCount[i] && (best == kCallSlots || t.callCount[i] > t.callCount[best])) best = i;
		if (best == kCallSlots) break;
		char leaf[64] = {}, caller[64] = {}, up[64] = {};
		wut_OSGetSymbolName(t.callPc[best], leaf, sizeof(leaf));
		if (code_addr(t.callLr[best])) wut_OSGetSymbolName(t.callLr[best], caller, sizeof(caller));
		if (t.callUp[best]) wut_OSGetSymbolName(t.callUp[best], up, sizeof(up));
		port_log("%s via %5.1f pct %s <- %s <- %s\n", tag, t.samples ? 100.0f * t.callCount[best] / t.samples : 0.0f,
		         short_name(leaf), short_name(caller), t.callUp[best] ? short_name(up) : "?");
		t.callCount[best] = 0;
	}
	{
		// Inclusive: per function, the main-thread samples with it on the stack.
		static Func inc[512];
		uint32_t ninc = 0;
		for (uint32_t i = 0; i < kIncSlots; ++i) {
			if (!t.incPc[i]) continue;
			char name[64] = {};
			wut_OSGetSymbolName(t.incPc[i], name, sizeof(name));
			uint32_t f = 0;
			while (f < ninc && std::strcmp(inc[f].name, name) != 0) ++f;
			if (f == ninc) {
				if (ninc == 512) continue;
				std::memcpy(inc[f].name, name, sizeof(name));
				inc[f].count = 0;
				++ninc;
			}
			inc[f].count += t.incCount[i];
		}
		port_log("%s inclusive (of %u main-thread samples, dropped %u):\n", tag, (unsigned)t.main, (unsigned)t.incDropped);
		for (int rank = 0; rank < 45; ++rank) {
			uint32_t best = ninc;
			for (uint32_t f = 0; f < ninc; ++f)
				if (inc[f].count && (best == ninc || inc[f].count > inc[best].count)) best = f;
			if (best == ninc) break;
			port_log("%s inc %5.1f pct %s\n", tag, t.main ? 100.0f * inc[best].count / t.main : 0.0f, inc[best].name);
			inc[best].count = 0;
		}
	}
	for (int rank = 0; rank < 30; ++rank) {
		uint32_t best = nfuncs;
		for (uint32_t f = 0; f < nfuncs; ++f)
			if (funcs[f].count && (best == nfuncs || funcs[f].count > funcs[best].count)) best = f;
		if (best == nfuncs) break;
		port_log("%s %2d %5.1f%% %5u %s\n", tag, rank, t.samples ? 100.0f * funcs[best].count / t.samples : 0.0f,
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
	/* The sampler costs CPU every millisecond: only with the SD marker
	 * "perfdiag", or "cpuprof" for the sampler alone (perfdiag also turns
	 * gx2prod's renderer paths off; cpuprof keeps them, for core 1 as shipped).
	 * The frame count above still feeds the hang check. */
	static int s_enabled = -1;
	if (s_enabled < 0) {
		s_mainThread = wut_OSGetCurrentThread(); // the hang check needs it either way
		s_enabled = sms_wiiu_sd_marker("perfdiag") || sms_wiiu_sd_marker("cpuprof");
		port_log("[prof] sampler %s (SD marker perfdiag or cpuprof)\n", s_enabled ? "on" : "off");
	}
	if (!s_enabled)
		return;
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
	report(s_tables[done], "[prof]", s_mainThread);
	if (s_wrunning) {
		const uint32_t wdone = s_wactive & 1;
		s_wactive = wdone ^ 1;
		report(s_wtables[wdone], "[wprof]", s_workerThread);
	}
}

/* From the GX2 producer worker's thread at its start: with the SD marker
 * "workerprof", sample its core too (an alarm fires on the core that set it). */
extern "C" void sms_wiiu_profiler_worker_start(void)
{
	if (s_wrunning || !sms_wiiu_sd_marker("workerprof")) return;
	std::memset(s_wtables, 0, sizeof(s_wtables));
	s_workerThread = wut_OSGetCurrentThread();
	wut_OSCreateAlarm(s_walarm);
	s_wrunning = wut_OSSetPeriodicAlarm(s_walarm, wut_OSGetTime() + kPeriod, kPeriod, on_walarm) != 0;
	port_log("[wprof] worker sampler %s on thread %p\n", s_wrunning ? "on" : "failed", s_workerThread);
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
	if (s_wrunning) {
		wut_OSCancelAlarm(s_walarm);
		s_wrunning = false;
	}
	if (!s_running) return;
	wut_OSCancelAlarm(s_alarm);
	s_running = false;
}
