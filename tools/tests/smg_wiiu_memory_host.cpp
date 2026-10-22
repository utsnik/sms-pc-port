#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "smg_arena.h"

alignas(64) static unsigned char s_mem1[24u * 1024u * 1024u];
alignas(64) static unsigned char s_mem2[64u * 1024u * 1024u];
alignas(64) static unsigned char s_port_pool[0x4000];

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
    unsigned char* mem1_start = s_mem1;
    unsigned char* mem1_end = s_mem1 + sizeof(s_mem1);
    unsigned char* mem2_start = s_mem2 + 0x4000;
    unsigned char* mem2_end = s_mem2 + sizeof(s_mem2);
    check(mem1_end - mem1_start == 24u * 1024u * 1024u, "MEM1 size");
    check(mem2_end - s_mem2 == 64u * 1024u * 1024u, "MEM2 size");

    unsigned char* mem1_lo = mem1_start;
    void* mem1 = smg_wiiu::alloc_lo(mem1_lo, mem1_end, mem1_start, mem1_end, 1, 1);
    check(mem1 && address(mem1) % 32 == 0, "MEM1 allocation alignment");
    check(address(mem1_lo) % 32 == 0, "MEM1 low bound alignment");

    unsigned char* low = mem2_start;
    unsigned char* low_hi = mem2_start + 0x120;
    void* low_result = smg_wiiu::alloc_lo(low, low_hi, mem2_start, mem2_end, 0x101, 64);
    check(!low_result, "low allocation crossing Hi must fail");
    check(low == mem2_start, "failed low allocation moved Lo");

    unsigned char* high_lo = mem2_start + 0x90;
    unsigned char* high = mem2_start + 0x120;
    void* high_result = smg_wiiu::alloc_hi(high_lo, high, mem2_start, 0x80, 64);
    check(!high_result, "high allocation crossing Lo must fail");
    check(high == mem2_start + 0x120, "failed high allocation moved Hi");
    check(address(high) % 32 == 0, "high bound alignment");

    // Replay the Wii boot split: JKRHeap::initArena consumes MEM1, then
    // HeapMemoryWatcher takes the alt-ARAM window from the initial MEM2 low
    // mark before ARInit records that same base.
    unsigned char* mem1_root_lo = mem1_start;
    void* mem1_root = smg_wiiu::alloc_lo(mem1_root_lo, mem1_end, mem1_start,
                                         mem1_end, 0x2000, 64);
    check(mem1_root != nullptr, "MEM1 root heap allocation");
    unsigned char* mem2_lo = mem2_start;
    unsigned char* mem2_hi = mem2_end;
    unsigned char* alt_base = mem2_lo;
    smg_wiiu::set_hi(mem2_lo, mem2_hi, mem2_start, mem2_end,
                     mem2_lo + 0x00E00000);
    check(alt_base == mem2_start, "alt-ARAM base moved before ARInit");
    check(mem2_hi - alt_base == 0x00E00000,
          "alt-ARAM window is not exactly 0xE00000");
    unsigned char* port_lo = s_port_pool;
    void* dol_buffer = smg_wiiu::alloc_lo(port_lo, s_port_pool + sizeof(s_port_pool),
                                          s_port_pool, s_port_pool + sizeof(s_port_pool),
                                          0x1000, 64);
    check(dol_buffer != nullptr, "private port pool dol buffer");
    check(address(dol_buffer) < address(s_port_pool) + sizeof(s_port_pool) &&
              (address(dol_buffer) + 0x1000 <= address(mem1_start) ||
               address(dol_buffer) >= address(mem1_end)) &&
              (address(dol_buffer) + 0x1000 <= address(mem2_start) ||
               address(dol_buffer) >= address(mem2_end)),
          "port allocation overlaps a Galaxy-visible arena");

    unsigned char* setter_lo = mem2_start;
    unsigned char* setter_hi = mem2_start + 0x80;
    smg_wiiu::set_lo(setter_lo, setter_hi, mem2_start, mem2_end,
                     mem2_start + 0x100);
    check(setter_lo == setter_hi, "SetArenaLo crossed Hi");
    smg_wiiu::set_hi(setter_lo, setter_hi, mem2_start, mem2_end, mem2_start);
    check(setter_hi == setter_lo, "SetArenaHi crossed Lo");

    std::puts("memory-host: PASS: boot arena split, allocator bounds, and private port pool");
    return 0;
}
