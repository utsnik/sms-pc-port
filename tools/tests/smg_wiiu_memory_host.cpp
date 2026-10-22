#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "smg_arena.h"

alignas(64) static unsigned char s_mem1[24u * 1024u * 1024u];
alignas(64) static unsigned char s_mem2[64u * 1024u * 1024u];

static void fail(const char* message)
{
    std::fprintf(stderr, "memory-host: FAIL: %s\n", message);
    std::exit(1);
}

static void check(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

static uintptr_t address(const void* pointer)
{
    return reinterpret_cast<uintptr_t>(pointer);
}

int main()
{
    smg_wiiu::ArenaState arenas;
    smg_wiiu::init(arenas, s_mem1, sizeof(s_mem1), s_mem2, sizeof(s_mem2));
    unsigned char* mem1_start = smg_wiiu::mem1_lo(arenas);
    unsigned char* mem1_end = smg_wiiu::mem1_hi(arenas);
    unsigned char* mem2_start = smg_wiiu::mem2_lo(arenas);
    unsigned char* mem2_end = smg_wiiu::mem2_hi(arenas);
    check(mem1_end - mem1_start == 24u * 1024u * 1024u, "MEM1 size");
    check(mem2_end - mem2_start == 64u * 1024u * 1024u - 0x4000,
          "MEM2 arena mark");

    void* mem1 = smg_wiiu::alloc_mem1_lo(arenas, 1, 1);
    check(mem1 && address(mem1) % 32 == 0, "MEM1 allocation alignment");
    check(address(smg_wiiu::mem1_lo(arenas)) % 32 == 0,
          "MEM1 low bound alignment");

    smg_wiiu::ArenaState bounded = arenas;
    smg_wiiu::set_mem2_lo(bounded, mem2_start);
    smg_wiiu::set_mem2_hi(bounded, mem2_start + 0x120);
    void* low_result = smg_wiiu::alloc_mem2_lo(bounded, 0x101, 64);
    check(!low_result, "low allocation crossing Hi must fail");
    check(smg_wiiu::mem2_lo(bounded) == mem2_start,
          "failed low allocation moved Lo");

    smg_wiiu::set_mem2_lo(bounded, mem2_start + 0x90);
    smg_wiiu::set_mem2_hi(bounded, mem2_start + 0x120);
    void* high_result = smg_wiiu::alloc_mem2_hi(bounded, 0x80, 64);
    check(!high_result, "high allocation crossing Lo must fail");
    check(smg_wiiu::mem2_hi(bounded) == mem2_start + 0x120,
          "failed high allocation moved Hi");
    check(address(smg_wiiu::mem2_hi(bounded)) % 32 == 0,
          "high bound alignment");

    // Replay the real Wii boot split through the same arena functions that
    // os_core.cpp calls: JKRHeap::initArena takes OSGetArenaLo/Hi (MEM1),
    // then HeapMemoryWatcher takes the alt-ARAM window from MEM2 low and
    // ARInit records that base and remaining size.
    unsigned char* root_lo = smg_wiiu::arena_lo(arenas);
    unsigned char* root_hi = smg_wiiu::arena_hi(arenas);
    void* mem1_root = root_lo;
    smg_wiiu::set_arena_lo(arenas, root_hi);
    smg_wiiu::set_arena_hi(arenas, root_hi);
    check(mem1_root != nullptr && smg_wiiu::arena_lo(arenas) == root_hi,
          "MEM1 root heap consumes the MEM1 arena");

    unsigned char* alt_base = smg_wiiu::mem2_lo(arenas);
    smg_wiiu::set_mem2_hi(arenas, alt_base + smg_wiiu::kAramWindowBytes);
    check(alt_base == mem2_start, "alt-ARAM base moved before ARInit");
    check(smg_wiiu::mem2_hi(arenas) - alt_base == smg_wiiu::kAramWindowBytes,
          "alt-ARAM window is not exactly 0xE00000");
    unsigned char* gddr3_start = smg_wiiu::mem2_hi(arenas);
    check(gddr3_start == alt_base + smg_wiiu::kAramWindowBytes,
          "MEM2 GDDR3 heap does not follow alt-ARAM");
    void* dol_buffer = std::malloc(0x1000);
    check(dol_buffer != nullptr, "malloc'd port DOL buffer");
    check(address(dol_buffer) + 0x1000 <= address(mem1_start) ||
              address(dol_buffer) >= address(mem1_end),
          "port allocation overlaps a Galaxy-visible arena");
    check(address(dol_buffer) + 0x1000 <= address(mem2_start) ||
              address(dol_buffer) >= address(mem2_end),
          "port allocation overlaps a Galaxy-visible arena");
    std::free(dol_buffer);

    // If OSGetArenaLo is temporarily pointed back at MEM2, the root heap
    // assertion above fails before any later MEM2 split can hide the mistake.
    smg_wiiu::set_arena_lo(arenas, smg_wiiu::arena_hi(arenas));
    check(smg_wiiu::arena_lo(arenas) == smg_wiiu::arena_hi(arenas),
          "SetArenaLo crossed Hi");

    std::puts("memory-host: PASS: shared boot arena split, allocator bounds, and malloc port block");
    return 0;
}
