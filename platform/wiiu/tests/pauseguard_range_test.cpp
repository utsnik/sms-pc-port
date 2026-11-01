#include "../pauseguard_range.h"

#include <cassert>
#include <cstdint>

int main()
{
    const SmsPauseGuardRange ranges[] = {{0x1000, 0x100}, {0x2000, 0x20}};
    assert(sms_pauseguard_range_contains(ranges, 2, 0x1000, 1));
    assert(sms_pauseguard_range_contains(ranges, 2, 0x10f0, 0x10));
    assert(sms_pauseguard_range_contains(ranges, 2, 0x2000, 0x20));
    assert(!sms_pauseguard_range_contains(ranges, 2, 0x0fff, 2));
    assert(!sms_pauseguard_range_contains(ranges, 2, 0x10f0, 0x11));
    assert(!sms_pauseguard_range_contains(ranges, 2, 0x2020, 1));
    assert(!sms_pauseguard_range_contains(ranges, 2, UINTPTR_MAX - 3, 8));
    assert(sms_pauseguard_range_contains(ranges, 2, UINTPTR_MAX, 0));
    return 0;
}
