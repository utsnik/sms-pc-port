#ifndef SMG_WIIU_ARENA_H
#define SMG_WIIU_ARENA_H

#include <cstdint>

namespace smg_wiiu {

constexpr uint32_t kMem2ArenaOffset = 0x00004000u;
constexpr uint32_t kAramBaseOffset = 0x00004000u;
/* Retail Wii aralt (ruling R18): createRootHeap carves this MEM2 window and
 * ARInit reports it whole as __AR_Size; ARAlloc hands out offsets from
 * ARGetBaseAddress() = 0x4000 without a bound. */
constexpr uint32_t kAramWindowBytes = 0x00E00000u;

/* The DSP's VARAM read (runtime.cpp port_varam_ptr): base + offset resolves
 * inside [base + kAramBaseOffset, base + min(size, kAramWindowBytes)). Below
 * 0x4000 is aralt's reserved area, never handed out by ARAlloc. A read past
 * the window would alias the GDDR3 heap after it, so it resolves to nothing
 * and *beyond is set for the caller's one-time log. */
inline const uint8_t* varam_lookup(uint32_t base, uint32_t size, uint32_t addr,
                                   bool* beyond)
{
    if (beyond)
        *beyond = false;
    if (!base || addr < base)
        return nullptr;
    const uint32_t window = size < kAramWindowBytes ? size : kAramWindowBytes;
    const uint32_t offset = addr - base;
    if (offset >= window) {
        if (beyond)
            *beyond = true;
        return nullptr;
    }
    if (offset < kAramBaseOffset)
        return nullptr;
    return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr));
}

struct ArenaState {
    uint8_t* mem1_start;
    uint8_t* mem1_end;
    uint8_t* mem1_lo;
    uint8_t* mem1_hi;
    uint8_t* mem2_start;
    uint8_t* mem2_end;
    uint8_t* mem2_lo;
    uint8_t* mem2_hi;
};

inline uint8_t* align_up(uint8_t* pointer, uint32_t alignment)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    return reinterpret_cast<uint8_t*>((value + alignment - 1) &
                                      ~(uintptr_t)(alignment - 1));
}

inline uint8_t* align_down(uint8_t* pointer, uint32_t alignment)
{
    uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    return reinterpret_cast<uint8_t*>(value & ~(uintptr_t)(alignment - 1));
}

inline uint32_t effective_alignment(uint32_t alignment)
{
    return alignment >= 32 && !(alignment & (alignment - 1)) ? alignment : 32;
}

inline void init(ArenaState& state, uint8_t* mem1_base, uint32_t mem1_size,
                 uint8_t* mem2_base, uint32_t mem2_size)
{
    state.mem1_start = align_up(mem1_base, 32);
    state.mem1_end = align_down(mem1_base + mem1_size, 32);
    state.mem1_lo = state.mem1_start;
    state.mem1_hi = state.mem1_end;
    state.mem2_start = align_up(mem2_base + kMem2ArenaOffset, 32);
    state.mem2_end = align_down(mem2_base + mem2_size, 32);
    state.mem2_lo = state.mem2_start;
    state.mem2_hi = state.mem2_end;
}

inline uint8_t* mem1_lo(const ArenaState& state) { return state.mem1_lo; }
inline uint8_t* mem1_hi(const ArenaState& state) { return state.mem1_hi; }
inline uint8_t* mem2_lo(const ArenaState& state) { return state.mem2_lo; }
inline uint8_t* mem2_hi(const ArenaState& state) { return state.mem2_hi; }
inline uint8_t* arena_lo(const ArenaState& state) { return state.mem1_lo; }
inline uint8_t* arena_hi(const ArenaState& state) { return state.mem1_hi; }

inline void set_lo(uint8_t*& lo, uint8_t* hi, uint8_t* start, uint8_t* end,
                   void* requested)
{
    (void)hi;
    uint8_t* value = align_up(static_cast<uint8_t*>(requested), 32);
    if (value < start)
        value = start;
    if (value > end)
        value = end;
    if (value > hi)
        value = hi;
    lo = value;
}

inline void set_hi(uint8_t* lo, uint8_t*& hi, uint8_t* start, uint8_t* end,
                   void* requested)
{
    (void)lo;
    uint8_t* value = align_down(static_cast<uint8_t*>(requested), 32);
    if (value < start)
        value = start;
    if (value > end)
        value = end;
    if (value < lo)
        value = lo;
    hi = value;
}

inline void* alloc_lo(uint8_t*& lo, uint8_t* hi, uint8_t* start, uint8_t* end,
                      uint32_t size, uint32_t alignment)
{
    (void)end;
    alignment = effective_alignment(alignment);
    if (lo > hi)
        return nullptr;
    uint8_t* pointer = align_up(lo, alignment);
    if (pointer < start || pointer > hi || size > static_cast<uint32_t>(hi - pointer))
        return nullptr;
    uint8_t* next_lo = align_up(pointer + size, alignment);
    if (next_lo > hi)
        return nullptr;
    lo = next_lo;
    return pointer;
}

inline void* alloc_hi(uint8_t* lo, uint8_t*& hi, uint8_t* start, uint32_t size,
                      uint32_t alignment)
{
    alignment = effective_alignment(alignment);
    if (lo > hi || size > static_cast<uint32_t>(hi - lo))
        return nullptr;
    uint8_t* pointer = align_down(hi - size, alignment);
    if (pointer < lo || pointer < start ||
        size > static_cast<uint32_t>(hi - pointer))
        return nullptr;
    uint8_t* next_hi = pointer;
    if (next_hi < lo)
        return nullptr;
    hi = next_hi;
    return pointer;
}

inline void set_arena_lo(ArenaState& state, void* requested)
{
    set_lo(state.mem1_lo, state.mem1_hi, state.mem1_start, state.mem1_end,
           requested);
}

inline void set_arena_hi(ArenaState& state, void* requested)
{
    set_hi(state.mem1_lo, state.mem1_hi, state.mem1_start, state.mem1_end,
           requested);
}

inline void set_mem2_lo(ArenaState& state, void* requested)
{
    set_lo(state.mem2_lo, state.mem2_hi, state.mem2_start, state.mem2_end,
           requested);
}

inline void set_mem2_hi(ArenaState& state, void* requested)
{
    set_hi(state.mem2_lo, state.mem2_hi, state.mem2_start, state.mem2_end,
           requested);
}

inline void* alloc_mem1_lo(ArenaState& state, uint32_t size, uint32_t alignment)
{
    return alloc_lo(state.mem1_lo, state.mem1_hi, state.mem1_start,
                    state.mem1_end, size, alignment);
}

inline void* alloc_mem2_lo(ArenaState& state, uint32_t size, uint32_t alignment)
{
    return alloc_lo(state.mem2_lo, state.mem2_hi, state.mem2_start,
                    state.mem2_end, size, alignment);
}

inline void* alloc_mem2_hi(ArenaState& state, uint32_t size, uint32_t alignment)
{
    return alloc_hi(state.mem2_lo, state.mem2_hi, state.mem2_start, size,
                    alignment);
}

inline void* alloc_arena_lo(ArenaState& state, uint32_t size, uint32_t alignment)
{
    return alloc_mem1_lo(state, size, alignment);
}

inline void* alloc_arena_hi(ArenaState& state, uint32_t size, uint32_t alignment)
{
    return alloc_hi(state.mem1_lo, state.mem1_hi, state.mem1_start, size,
                    alignment);
}

} // namespace smg_wiiu

#endif
