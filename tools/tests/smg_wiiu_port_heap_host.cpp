// Host harness for ruling R17: platform/wiiu/port_heap.cpp's operator
// new/delete wraps, compiled as shipped for PORT_GAME_SMG against stub
// coreinit headers (test_smg_wiiu_port_heap.py writes them). The stubs count
// the system ("malloc") heap; the __real_ symbols stand for Galaxy's JKRHeap.cpp
// operator new/delete ("game" allocator).
#include "port_heap.h"
#include "host_alloc.h"

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int s_system_allocs, s_system_frees, s_game_allocs, s_game_frees;
thread_local void* s_specific[OS_THREAD_SPECIFIC_13 + 1];

void fail(const char* message)
{
    std::fprintf(stderr, "port-heap-host: FAIL: %s\n", message);
    std::exit(1);
}

void check(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

void* game_alloc(size_t n)
{
    ++s_game_allocs;
    return std::malloc(n);
}

void game_free(void* p)
{
    ++s_game_frees;
    std::free(p);
}

} // namespace

void* MEMAllocFromDefaultHeapEx(uint32_t size, int alignment)
{
    ++s_system_allocs;
    void* p = nullptr;
    return posix_memalign(&p, size_t(alignment), size) == 0 ? p : nullptr;
}

void MEMFreeToDefaultHeap(void* p)
{
    ++s_system_frees;
    std::free(p);
}

void* OSGetThreadSpecific(OSThreadSpecificID id) { return s_specific[id]; }
void OSSetThreadSpecific(OSThreadSpecificID id, void* value) { s_specific[id] = value; }

extern "C" {
uint32_t sms_wiiu_heap_free(void) { return 0; }
void port_log(const char*, ...) {}

void* __real__Znwj(size_t n) { return game_alloc(n); }
void* __real__Znaj(size_t n) { return game_alloc(n); }
void __real__ZdlPv(void* p) { game_free(p); }
void __real__ZdaPv(void* p) { game_free(p); }
void __real__ZdlPvj(void* p, size_t) { game_free(p); }
void __real__ZdaPvj(void* p, size_t) { game_free(p); }

void* __wrap__Znwj(size_t);
void* __wrap__Znaj(size_t);
void __wrap__ZdlPv(void*);
void __wrap__ZdaPv(void*);
void __wrap__ZdlPvj(void*, size_t);
void __wrap__ZdaPvj(void*, size_t);
}

int main()
{
    // Unscoped: Galaxy's operator new (JKR heaps once the root heap exists).
    void* game = __wrap__Znwj(24);
    check(game && s_game_allocs == 1 && s_system_allocs == 0,
          "unscoped new did not reach the game allocator");

    void* port = nullptr;
    void* port_array = nullptr;
    void* port_sized = nullptr;
    {
        SMS_PORT_HEAP_SCOPE();
        port = __wrap__Znwj(24);
        port_array = __wrap__Znaj(40);
        port_sized = __wrap__Znaj(12);
        // Freeing a game block inside a scope still returns it to the game.
        __wrap__ZdlPv(game);
    }
    check(port && port_array && port_sized, "scoped new returned null");
    check(s_system_allocs == 3 && s_game_allocs == 1,
          "scoped new did not reach the system (malloc) heap");
    check(reinterpret_cast<uintptr_t>(port) % 32 == 0, "scoped block is not 32-byte aligned");
    check(s_game_frees == 1 && s_system_frees == 0,
          "delete of a game block inside a scope did not reach the game allocator");

    // Scoped blocks go back to the system heap whatever scope the delete
    // runs in (port_free recognises them by their tag).
    __wrap__ZdlPv(port);
    __wrap__ZdaPv(port_array);
    __wrap__ZdaPvj(port_sized, 12);
    check(s_system_frees == 3 && s_game_frees == 1,
          "delete of a scoped block did not return it to the system heap");

    void* game_sized = __wrap__Znwj(8);
    __wrap__ZdlPvj(game_sized, 8);
    void* game_array_sized = __wrap__Znaj(8);
    __wrap__ZdaPvj(game_array_sized, 8);
    check(s_game_allocs == 3 && s_game_frees == 3 && s_system_frees == 3,
          "sized delete of a game block left the game allocator");

    // The scope is per thread: another thread allocating while this one is
    // inside a scope still uses the game allocator.
    {
        SMS_PORT_HEAP_SCOPE();
        void* other = nullptr;
        std::thread([&other] { other = __wrap__Znwj(16); }).join();
        check(s_game_allocs == 4 && s_system_allocs == 3,
              "a scope leaked into another thread");
        __wrap__ZdlPv(other);
    }
    check(s_game_frees == 4, "other-thread game block was not freed to the game");

    // port_host_alloc (games/smg/platform/wiiu/runtime.cpp) alignment.
    check(smg_wiiu::host_alloc_alignment(0) == 8, "alignment 0");
    check(smg_wiiu::host_alloc_alignment(4) == 8, "alignment 4");
    check(smg_wiiu::host_alloc_alignment(32) == 32, "alignment 32");
    check(smg_wiiu::host_alloc_alignment(-32) == 32, "alignment -32 (tail allocation)");
    check(smg_wiiu::host_alloc_alignment(48) == 64, "alignment 48 rounds to a power of two");
    check(smg_wiiu::host_alloc_alignment(0x1000) == 0x1000, "alignment 4096");

    std::puts("port-heap-host: PASS: scoped new -> system heap, unscoped -> game allocator, "
              "delete pairs by tag (incl. _ZdaPvj), per-thread scope, host alignment");
    return 0;
}
