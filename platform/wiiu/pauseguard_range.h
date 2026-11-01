#ifndef SMS_WIIU_PAUSEGUARD_RANGE_H
#define SMS_WIIU_PAUSEGUARD_RANGE_H

#include <stddef.h>
#include <stdint.h>

/* Kept independent of Wii U headers so the overflow-safe range predicate can
 * be exercised by the host tests.  A zero-length range is valid at any
 * address; callers use non-zero lengths for actual GPU targets. */
struct SmsPauseGuardRange {
    uintptr_t begin;
    uint64_t size;
};

static inline bool sms_pauseguard_range_contains(const SmsPauseGuardRange* ranges,
                                                 size_t count, uintptr_t begin,
                                                 uint64_t size)
{
    if (!size)
        return true;
    for (size_t i = 0; i < count; ++i) {
        const uint64_t rangeBegin = uint64_t(ranges[i].begin);
        const uint64_t rangeEnd = rangeBegin + ranges[i].size;
        const uint64_t requestBegin = uint64_t(begin);
        const uint64_t requestEnd = requestBegin + size;
        if (rangeEnd < rangeBegin || requestEnd < requestBegin)
            continue;
        if (requestBegin >= rangeBegin && requestEnd <= rangeEnd)
            return true;
    }
    return false;
}

#endif
