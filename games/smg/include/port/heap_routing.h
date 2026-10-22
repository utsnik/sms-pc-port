// Decides whether a C++ allocation belongs in the game's JKRHeap tree or in the host heap.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// True once Galaxy's JKR root heap exists. Port code allocating after that point
// is routed to the system heap before it gets here, by SMS_PORT_HEAP_SCOPE and
// the operator new/delete wraps in platform/wiiu/port_heap.cpp.
int port_use_game_heap(void);

// True when p lies inside the emulated MEM1/MEM2 arenas that back the JKR heaps.
int port_is_game_heap_ptr(const void* p);

void* port_host_alloc(size_t size, int align);
void port_host_free(void* p);

#ifdef __cplusplus
}
#endif
