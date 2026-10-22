#ifndef SMG_WIIU_ARENA_H
#define SMG_WIIU_ARENA_H

#include <cstdint>

namespace smg_wiiu {

constexpr uint32_t kMem2ArenaOffset = 0x00004000u;
constexpr uint32_t kAramWindowBytes = 0x00E00000u;

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

} // namespace smg_wiiu

#endif
