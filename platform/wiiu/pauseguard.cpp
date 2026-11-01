#include "pauseguard.h"

#include "runtime.h"

#include <coreinit/memory.h>
#include <whb/log.h>

#include <stddef.h>
#include <stdint.h>

namespace {

constexpr uint32_t kPhysMask = 0x3fffffffu;
constexpr unsigned kRangeCount = 3; // MEM2 arena, usable MEM1, foreground heap
constexpr unsigned kSkipLogLimit = 96;
constexpr unsigned kTraceLogLimit = 4096;
constexpr uint32_t kTraceFrames = 120;

bool s_initialized;
bool s_enabled;
SmsPauseGuardRange s_ranges[kRangeCount];
void* s_scan[2];
uint32_t s_scanSize[2];
uint32_t s_skipLogs;
uint32_t s_traceLogs;
volatile uint32_t s_traceFrame = kTraceFrames;
volatile uint32_t s_traceArmed;

uintptr_t physRangeBegin(uintptr_t begin)
{
    return begin & kPhysMask;
}

bool physicalContains(uint32_t phys, uint32_t size)
{
    if (!size)
        return true;
    if (phys <= sms_wiiu_mem2_size && uint64_t(phys) + size <= sms_wiiu_mem2_size)
        return true; // address-zero GX window: MEM2 physical offset
    for (unsigned i = 0; i < kRangeCount; ++i) {
        const uintptr_t begin = physRangeBegin(s_ranges[i].begin);
        const uint64_t end = uint64_t(begin) + s_ranges[i].size;
        const uint64_t requestEnd = uint64_t(phys) + size;
        if (end >= begin && requestEnd >= phys && uint64_t(phys) >= begin && requestEnd <= end)
            return true;
    }
    return false;
}

uint32_t pointerPhys(const void* ptr)
{
    return ptr ? uint32_t(reinterpret_cast<uintptr_t>(ptr) & kPhysMask) : 0;
}

void skip(const char* op, const void* p0, uint32_t phys0, uint32_t size0,
          const void* p1, uint32_t phys1, uint32_t size1, uintptr_t caller)
{
    if (s_skipLogs >= kSkipLogLimit)
        return;
    ++s_skipLogs;
    WHBLogPrintf("[pauseguard] SKIP op=%s p0=%p phys0=%08x n0=%u p1=%p phys1=%08x n1=%u caller=%08x",
                 op, p0, unsigned(phys0), unsigned(size0), p1, unsigned(phys1), unsigned(size1),
                 unsigned(caller));
}

void rangeLog(const char* op, const void* ptr, uint32_t size, uintptr_t caller)
{
    skip(op, ptr, pointerPhys(ptr), size, nullptr, 0, 0, caller);
}

void refreshForeground()
{
    uint32_t address = 0, size = 0;
    OSGetForegroundBucket(&address, &size);
    s_ranges[2] = {uintptr_t(address), uint64_t(size)};
}

} // namespace

extern "C" {

void sms_wiiu_pauseguard_init(void)
{
    if (s_initialized)
        return;
    s_initialized = true;
    s_enabled = sms_wiiu_sd_marker("pauseguard") != 0;
    if (!s_enabled)
        return;
    s_ranges[0] = {reinterpret_cast<uintptr_t>(sms_wiiu_mem2_base), sms_wiiu_mem2_size};
    s_ranges[1] = {reinterpret_cast<uintptr_t>(port_mem1_base), port_mem1_size};
    refreshForeground();
    WHBLogPrintf("SMS Wii U: [pauseguard] on MEM2=%p+%u MEM1=%p+%u foreground=%08x+%u",
                 sms_wiiu_mem2_base, unsigned(sms_wiiu_mem2_size), port_mem1_base,
                 unsigned(port_mem1_size), unsigned(s_ranges[2].begin), unsigned(s_ranges[2].size));
}

int sms_wiiu_pauseguard_enabled(void)
{
    return s_enabled ? 1 : 0;
}

void sms_wiiu_pauseguard_refresh_ranges(void)
{
    if (s_enabled)
        refreshForeground();
}

int sms_wiiu_pauseguard_check_range(const char* op, const void* ptr, uint32_t size,
                                    uintptr_t caller)
{
    if (!s_enabled || !size)
        return 1;
    if (ptr && sms_pauseguard_range_contains(s_ranges, kRangeCount,
                                             reinterpret_cast<uintptr_t>(ptr), size))
        return 1;
    rangeLog(op, ptr, size, caller);
    return 0;
}

int sms_wiiu_pauseguard_check_translation(const char* op, const void* ptr, uint32_t phys,
                                          uint32_t size, uintptr_t caller)
{
    if (!s_enabled || !size)
        return 1;
    const bool pointerOk = ptr && sms_pauseguard_range_contains(s_ranges, kRangeCount,
                                                                 reinterpret_cast<uintptr_t>(ptr), size);
    const bool physicalOk = phys && physicalContains(phys & kPhysMask, size);
    if (pointerOk && physicalOk)
        return 1;
    skip(op, ptr, phys, size, nullptr, 0, 0, caller);
    return 0;
}

int sms_wiiu_pauseguard_check_surface(const char* op, const void* image, uint32_t imageSize,
                                      const void* mipmaps, uint32_t mipmapSize,
                                      uintptr_t caller)
{
    if (!s_enabled)
        return 1;
    if (!sms_wiiu_pauseguard_check_range(op, image, imageSize, caller))
        return 0;
    if (!sms_wiiu_pauseguard_check_range(op, mipmaps, mipmapSize, caller))
        return 0;
    return 1;
}

int sms_wiiu_pauseguard_check_scan(uint32_t target, const void* source, uint32_t sourceSize,
                                   uintptr_t caller)
{
    if (!s_enabled)
        return 1;
    if (!sms_wiiu_pauseguard_check_range("CopyColorBuffer source", source, sourceSize, caller))
        return 0;
    if (target < 2 && s_scan[target] && s_scanSize[target] &&
        !sms_wiiu_pauseguard_check_range("CopyColorBuffer scan", s_scan[target], s_scanSize[target], caller))
        return 0;
    return 1;
}

void sms_wiiu_pauseguard_note_scan_buffer(uint32_t target, void* ptr, uint32_t size)
{
    if (target < 2) {
        s_scan[target] = ptr;
        s_scanSize[target] = size;
    }
}

void sms_wiiu_pauseguard_trace(const char* op, const void* p0, uint32_t size0,
                               const void* p1, uint32_t size1,
                               const void* p2, uint32_t size2,
                               const void* p3, uint32_t size3,
                               uintptr_t caller)
{
    if (!s_enabled || !s_traceArmed || s_traceFrame >= kTraceFrames || s_traceLogs >= kTraceLogLimit)
        return;
    ++s_traceLogs;
    WHBLogPrintf("[pauseguard] f=%u op=%s p0=%p/%08x/%u p1=%p/%08x/%u p2=%p/%08x/%u p3=%p/%08x/%u caller=%08x",
                 unsigned(s_traceFrame), op, p0, unsigned(pointerPhys(p0)), unsigned(size0),
                 p1, unsigned(pointerPhys(p1)), unsigned(size1), p2, unsigned(pointerPhys(p2)),
                 unsigned(size2), p3, unsigned(pointerPhys(p3)), unsigned(size3), unsigned(caller));
}

void sms_wiiu_pauseguard_start(void)
{
    if (!s_enabled || s_traceArmed)
        return;
    s_traceFrame = 0;
    s_traceArmed = 1;
    s_traceLogs = 0;
    WHBLogPrintf("[pauseguard] START edge: tracing next %u presented frames", unsigned(kTraceFrames));
}

void sms_wiiu_pauseguard_present(void)
{
    if (!s_enabled || !s_traceArmed)
        return;
    if (++s_traceFrame >= kTraceFrames) {
        s_traceArmed = 0;
        WHBLogPrintf("[pauseguard] trace complete frames=%u records=%u", unsigned(kTraceFrames),
                     unsigned(s_traceLogs));
    }
    refreshForeground();
}

} // extern "C"
