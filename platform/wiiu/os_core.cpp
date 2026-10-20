#include "port_compat.h"
#include "port_os.h"
#include "runtime.h"

#include <dolphin/os.h>
#include <dolphin/os/OSAlloc.h>
#include <dolphin/os/OSCache.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSStopwatch.h>
#include <JSystem/JUtility/JUTAssert.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" {

void WHBProcStopRunning(void);

/* These aliases are the isolated wut ABI boundary.  The game-facing names
 * are remapped by CMake to sms_wiiu_* before this file is compiled. */
OSTime wut_OSGetTime(void) __asm__("OSGetTime");
OSTick wut_OSGetTick(void) __asm__("OSGetTick");
BOOL wut_OSDisableInterrupts(void) __asm__("OSDisableInterrupts");
BOOL wut_OSEnableInterrupts(void) __asm__("OSEnableInterrupts");
BOOL wut_OSRestoreInterrupts(BOOL) __asm__("OSRestoreInterrupts");
void wut_OSYieldThread(void) __asm__("OSYieldThread");
}

namespace {

uint8_t* s_arena_lo;
uint8_t* s_arena_hi;
int s_current_heap = -1;
const uint32_t kHeapCount = 16;
const uint32_t kCellHeader = 32;

struct HeapCell {
	HeapCell* next;
	uint32_t size;
	uint32_t used;
};

struct Heap {
	uint8_t* start;
	uint8_t* end;
	HeapCell* cells;
	bool live;
};

Heap s_heaps[kHeapCount];
port_irq_poll_fn s_irq_sources[8];
unsigned s_irq_source_count;

uint8_t* align_up(uint8_t* p, uint32_t align)
{
	uintptr_t n = reinterpret_cast<uintptr_t>(p);
	return reinterpret_cast<uint8_t*>((n + align - 1) & ~(uintptr_t)(align - 1));
}

uint8_t* align_down(uint8_t* p, uint32_t align)
{
	uintptr_t n = reinterpret_cast<uintptr_t>(p);
	return reinterpret_cast<uint8_t*>(n & ~(uintptr_t)(align - 1));
}

} // namespace

extern "C" {

void port_os_init(void)
{
	if (!sms_wiiu_mem2_base || !sms_wiiu_mem2_size)
		return;
	__OSBusClock = 162000000;
	__OSCoreClock = 486000000;
	__OSPhysicalMemSize = port_mem1_size;
	__OSSimulatedMemSize = port_mem1_size;
	s_arena_lo = align_up(sms_wiiu_mem2_base + 0x4000, 32);
	s_arena_hi = align_down(sms_wiiu_mem2_base + sms_wiiu_mem2_size, 32);
	std::memset(s_heaps, 0, sizeof(s_heaps));
	std::memset(sms_wiiu_mem2_base, 0, 0x4000);
	*reinterpret_cast<uint32_t*>(sms_wiiu_mem2_base + 0x28) = port_mem1_size;
	port_log("[os] MEM2 arena=%p..%p; MEM1 reported=%u bytes\n", s_arena_lo,
	         s_arena_hi, port_mem1_size);
}

void OSInit(void) {}
void* OSGetArenaLo(void) { return s_arena_lo; }
void* OSGetArenaHi(void) { return s_arena_hi; }
void OSSetArenaLo(void* p) { s_arena_lo = reinterpret_cast<uint8_t*>(p); }
void OSSetArenaHi(void* p) { s_arena_hi = reinterpret_cast<uint8_t*>(p); }

void* OSAllocFromArenaLo(u32 size, u32 align)
{
	if (!align || (align & (align - 1)))
		align = 32;
	uint8_t* p = align_up(s_arena_lo, align);
	if (p > s_arena_hi || size > static_cast<u32>(s_arena_hi - p)) {
		port_log("[os] arena-low overflow: %u bytes\n", size);
		return nullptr;
	}
	s_arena_lo = align_up(p + size, align);
	return p;
}

void* OSAllocFromArenaHi(u32 size, u32 align)
{
	if (!align || (align & (align - 1)))
		align = 32;
	uint8_t* p = align_down(s_arena_hi - size, align);
	if (p < s_arena_lo || size > static_cast<u32>(s_arena_hi - p)) {
		port_log("[os] arena-high overflow: %u bytes\n", size);
		return nullptr;
	}
	s_arena_hi = p;
	return p;
}

u32 OSGetPhysicalMemSize(void) { return port_mem1_size; }
u32 OSGetConsoleSimulatedMemSize(void) { return port_mem1_size; }

void* OSPhysicalToCached(u32 paddr)
{
	return paddr < sms_wiiu_mem2_size ? sms_wiiu_mem2_base + paddr : nullptr;
}
void* OSPhysicalToUncached(u32 paddr) { return OSPhysicalToCached(paddr); }
u32 OSCachedToPhysical(void* p)
{
	if (!p || p < sms_wiiu_mem2_base ||
	    static_cast<uint8_t*>(p) >= sms_wiiu_mem2_base + sms_wiiu_mem2_size)
		return 0;
	return static_cast<u32>(static_cast<uint8_t*>(p) - sms_wiiu_mem2_base);
}
u32 OSUncachedToPhysical(void* p) { return OSCachedToPhysical(p); }
void* OSCachedToUncached(void* p) { return p; }
void* OSUncachedToCached(void* p) { return p; }

void* OSInitAlloc(void* start, void* end, int max_heaps)
{
	(void)max_heaps;
	uint8_t* lo = align_up(reinterpret_cast<uint8_t*>(start), 32);
	uint8_t* hi = align_down(reinterpret_cast<uint8_t*>(end), 32);
	if (lo >= hi)
		return nullptr;
	return lo;
}

int OSCreateHeap(void* start, void* end)
{
	for (unsigned i = 0; i < kHeapCount; ++i) {
		if (s_heaps[i].live)
			continue;
		Heap& h = s_heaps[i];
		h.start = align_up(reinterpret_cast<uint8_t*>(start), 32);
		h.end = align_down(reinterpret_cast<uint8_t*>(end), 32);
		if (h.start >= h.end || static_cast<uint32_t>(h.end - h.start) <= kCellHeader)
			return -1;
		h.cells = reinterpret_cast<HeapCell*>(h.start);
		h.cells->next = nullptr;
		h.cells->size = static_cast<uint32_t>(h.end - h.start);
		h.cells->used = 0;
		h.live = true;
		return static_cast<int>(i);
	}
	return -1;
}

void OSDestroyHeap(int heap)
{
	if (heap >= 0 && heap < static_cast<int>(kHeapCount))
		s_heaps[heap].live = false;
}

int OSSetCurrentHeap(int heap)
{
	int old = s_current_heap;
	s_current_heap = heap;
	return old;
}

void* OSAllocFromHeap(int heap, unsigned long size)
{
	if (heap < 0 || heap >= static_cast<int>(kHeapCount) || !s_heaps[heap].live)
		return nullptr;
	uint32_t need = (static_cast<uint32_t>(size) + kCellHeader + 31) & ~31u;
	for (HeapCell* c = s_heaps[heap].cells; c; c = c->next) {
		if (c->used || c->size < need)
			continue;
		if (c->size >= need + 64) {
			HeapCell* n = reinterpret_cast<HeapCell*>(reinterpret_cast<uint8_t*>(c) + need);
			n->next = c->next;
			n->size = c->size - need;
			n->used = 0;
			c->next = n;
			c->size = need;
		}
		c->used = 1;
		return reinterpret_cast<uint8_t*>(c) + kCellHeader;
	}
	return nullptr;
}

void OSFreeToHeap(int heap, void* ptr)
{
	if (!ptr || heap < 0 || heap >= static_cast<int>(kHeapCount))
		return;
	HeapCell* cell = reinterpret_cast<HeapCell*>(reinterpret_cast<uint8_t*>(ptr) - kCellHeader);
	cell->used = 0;
	for (HeapCell* c = s_heaps[heap].cells; c; c = c->next) {
		while (!c->used && c->next && !c->next->used &&
		       reinterpret_cast<uint8_t*>(c) + c->size ==
		           reinterpret_cast<uint8_t*>(c->next)) {
			c->size += c->next->size;
			c->next = c->next->next;
		}
	}
}

long OSCheckHeap(int heap)
{
	if (heap < 0 || heap >= static_cast<int>(kHeapCount) || !s_heaps[heap].live)
		return -1;
	long free = 0;
	for (HeapCell* c = s_heaps[heap].cells; c; c = c->next)
		if (!c->used)
			free += c->size - kCellHeader;
	return free;
}

unsigned long OSReferentSize(void* ptr)
{
	return ptr ? reinterpret_cast<HeapCell*>(reinterpret_cast<uint8_t*>(ptr) - kCellHeader)->size -
	                 kCellHeader
	           : 0;
}

void OSDumpHeap(int heap)
{
	port_log("[os] heap=%d free=%ld\n", heap, OSCheckHeap(heap));
}

OSTime sms_wiiu_OSGetTime(void) { return wut_OSGetTime(); }
OSTick sms_wiiu_OSGetTick(void) { return wut_OSGetTick(); }

void OSHalt(const char* message)
{
	port_log("[os] halt: %s\n", message ? message : "(null)");
	WHBProcStopRunning();
}

static u32 s_sound_mode = OS_SOUND_MODE_STEREO;
static u32 s_progressive;
u32 OSGetSoundMode(void) { return s_sound_mode; }
void OSSetSoundMode(u32 mode) { s_sound_mode = mode; }
u32 OSGetProgressiveMode(void) { return s_progressive; }
void OSSetProgressiveMode(u32 mode) { s_progressive = mode; }
unsigned long OSGetResetCode(void) { return 0; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSGetResetButtonState(void) { return FALSE; }
void OSResetSystem(int reset, u32 code, BOOL menu)
{
	port_log("[os] reset=%d code=0x%x menu=%d\n", reset, code, menu);
	WHBProcStopRunning();
}

void port_irq_check(void)
{
	for (unsigned i = 0; i < s_irq_source_count; ++i)
		if (s_irq_sources[i])
			s_irq_sources[i]();
	sms_wiiu_dvd_pump();
}

void port_irq_kick(void) {}
void port_irq_add_source(port_irq_poll_fn fn)
{
	if (fn && s_irq_source_count < 8)
		s_irq_sources[s_irq_source_count++] = fn;
}

void port_os_run_lower(double) { wut_OSYieldThread(); }
double port_idle_seconds(void) { return 0.0; }
int port_vi_idle_advance(void) { return 0; }
int port_vi_deterministic(void) { return 0; }
s64 port_vi_virtual_ticks(void) { return port_time_ticks(); }
void port_audio_on_retrace(void) {}
void port_window_icon_init(void) {}
void port_irq_add_source_dummy(void) {}

} // extern "C"

namespace JUTAssertion {

u32 getSDevice(void) { return 0; }

void showAssert_f(u32 device, const char* file, int line, const char* fmt, ...)
{
	char message[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	port_log("[JUTAssert] device=%u %s:%d: %s\n", device, file, line,
	         message);
}

void setWarningMessage_f(u32 device, char* file, int line, const char* fmt, ...)
{
	char message[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	port_log("[JUTWarning] device=%u %s:%d: %s\n", device, file, line,
	         message);
}

void setLogMessage_f(u32 device, char* file, int line, const char* fmt, ...)
{
	char message[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	port_log("[JUTLog] device=%u %s:%d: %s\n", device, file, line, message);
}

} // namespace JUTAssertion
