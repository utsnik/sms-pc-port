#include "port_heap.h"

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>

#include <cstddef>
#include <cstdint>

/* The scope depth lives in a thread-specific slot, so another game thread
 * allocating while the GX thread is inside a scope still uses the JKR heaps.
 * (No TLS: elf2rpl cannot relocate it.) */
static const OSThreadSpecificID kScopeSlot = OS_THREAD_SPECIFIC_13;
static const uint32_t kTag = 0x504f5254u;  // "PORT"
static const uint32_t kHeader = 32;

extern "C" void sms_port_heap_enter(void)
{
	OSSetThreadSpecific(kScopeSlot, reinterpret_cast<void*>(
	    reinterpret_cast<uintptr_t>(OSGetThreadSpecific(kScopeSlot)) + 1));
}

extern "C" void sms_port_heap_leave(void)
{
	OSSetThreadSpecific(kScopeSlot, reinterpret_cast<void*>(
	    reinterpret_cast<uintptr_t>(OSGetThreadSpecific(kScopeSlot)) - 1));
}

static bool in_scope()
{
	return OSGetThreadSpecific(kScopeSlot) != nullptr;
}

static void* port_alloc(size_t size, size_t align)
{
	if (align < kHeader)
		align = kHeader;
	uint8_t* raw = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(uint32_t(size + align), int(align)));
	if (!raw)
		return nullptr;
	uint8_t* user = raw + align;
	reinterpret_cast<uint32_t*>(user)[-1] = kTag;
	reinterpret_cast<void**>(user)[-2] = raw;
	return user;
}

static bool port_free(void* p)
{
	if (!p || (reinterpret_cast<uintptr_t>(p) & 31) != 0)
		return false;
	uint32_t* tag = reinterpret_cast<uint32_t*>(p) - 1;
	if (*tag != kTag)
		return false;
	*tag = 0;
	MEMFreeToDefaultHeap(reinterpret_cast<void**>(p)[-2]);
	return true;
}

/* A scoped allocation the system heap could not satisfy falls back to the
 * game heaps; count those, and log the first few, since port data living in
 * a JKR heap is freed and reused under the port when the game resets it. */
extern "C" uint32_t sms_wiiu_heap_free(void);
extern "C" void port_log(const char* fmt, ...);
static uint32_t s_fallbacks, s_fallbackBytes;
static bool s_logging;
static void note_fallback(size_t n)
{
	++s_fallbacks;
	s_fallbackBytes += uint32_t(n);
	if (s_fallbacks <= 8 && !s_logging) {
		s_logging = true;
		port_log("[heap] port allocation of %u bytes fell back to the game heap (#%u, system free %u)\n",
		         unsigned(n), unsigned(s_fallbacks), unsigned(sms_wiiu_heap_free()));
		s_logging = false;
	}
}
/* A game allocation the JKR heaps could not satisfy returns null, and the
 * heap's own ":::cannot alloc memory" warning is dropped without a console, so
 * log the first few with their caller. */
static uint32_t s_gameNulls;
static void note_game_null(size_t n, void* caller)
{
	++s_gameNulls;
	if (s_gameNulls <= 16)
		port_log("[heap] game allocation of %u bytes returned null (#%u, caller %p)\n",
		         unsigned(n), unsigned(s_gameNulls), caller);
}
extern "C" uint32_t sms_port_heap_fallbacks(void) { return s_fallbacks; }
extern "C" uint32_t sms_port_heap_fallback_bytes(void) { return s_fallbackBytes; }

/* Linked with -Wl,--wrap for each symbol below. */
extern "C" {
void* __real__Znwj(size_t);
void* __real__Znaj(size_t);
void __real__ZdlPv(void*);
void __real__ZdaPv(void*);
void __real__ZdlPvj(void*, size_t);

/* With no exceptions a null result is written through, so a full system heap
 * falls back to the game heaps rather than returning null. */
void* __wrap__Znwj(size_t n)
{
	const bool scoped = in_scope();
	void* p = scoped ? port_alloc(n, 8) : nullptr;
	if (!p && scoped)
		note_fallback(n);
	if (!p && !(p = __real__Znwj(n)))
		note_game_null(n, __builtin_return_address(0));
	return p;
}
void* __wrap__Znaj(size_t n)
{
	const bool scoped = in_scope();
	void* p = scoped ? port_alloc(n, 8) : nullptr;
	if (!p && scoped)
		note_fallback(n);
	if (!p && !(p = __real__Znaj(n)))
		note_game_null(n, __builtin_return_address(0));
	return p;
}
void __wrap__ZdlPv(void* p) { if (!port_free(p)) __real__ZdlPv(p); }
void __wrap__ZdaPv(void* p) { if (!port_free(p)) __real__ZdaPv(p); }
void __wrap__ZdlPvj(void* p, size_t n) { if (!port_free(p)) __real__ZdlPvj(p, n); }
}
