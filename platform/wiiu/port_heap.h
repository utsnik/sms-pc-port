#pragma once
/* Port-side allocations (GX decoder caches, GX2 renderer buffers) must not
 * come out of the game's JKR heaps: the game overrides global operator new,
 * and a renderer std::vector growing inside a full or locked solid heap
 * crashed the first stage in JKRSolidHeap::alloc. Code inside a
 * PortHeapScope allocates from the system heap instead; operator delete
 * recognises those blocks by a tag, so either side may free them. */
#ifdef __WIIU__
extern "C" void sms_port_heap_enter(void);
extern "C" void sms_port_heap_leave(void);
struct PortHeapScope {
	PortHeapScope() { sms_port_heap_enter(); }
	~PortHeapScope() { sms_port_heap_leave(); }
	PortHeapScope(const PortHeapScope&) = delete;
	PortHeapScope& operator=(const PortHeapScope&) = delete;
};
#define SMS_PORT_HEAP_SCOPE() PortHeapScope sms_port_heap_scope_
#else
#define SMS_PORT_HEAP_SCOPE() ((void)0)
#endif
