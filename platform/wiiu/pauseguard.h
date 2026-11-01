#ifndef SMS_WIIU_PAUSEGUARD_H
#define SMS_WIIU_PAUSEGUARD_H

#include "pauseguard_range.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sms_wiiu_pauseguard_init(void);
int sms_wiiu_pauseguard_enabled(void);
void sms_wiiu_pauseguard_refresh_ranges(void);

/* These checks are no-ops when the marker is absent. */
int sms_wiiu_pauseguard_check_range(const char* op, const void* ptr, uint32_t size,
                                    uintptr_t caller);
int sms_wiiu_pauseguard_check_translation(const char* op, const void* ptr, uint32_t phys,
                                          uint32_t size, uintptr_t caller);
int sms_wiiu_pauseguard_check_surface(const char* op, const void* image, uint32_t imageSize,
                                      const void* mipmaps, uint32_t mipmapSize,
                                      uintptr_t caller);
int sms_wiiu_pauseguard_check_scan(uint32_t target, const void* source, uint32_t sourceSize,
                                   uintptr_t caller);
void sms_wiiu_pauseguard_note_scan_buffer(uint32_t target, void* ptr, uint32_t size);

void sms_wiiu_pauseguard_trace(const char* op, const void* p0, uint32_t size0,
                               const void* p1, uint32_t size1,
                               const void* p2, uint32_t size2,
                               const void* p3, uint32_t size3,
                               uintptr_t caller);

void sms_wiiu_pauseguard_start(void);
void sms_wiiu_pauseguard_present(void);

#ifdef __cplusplus
}
#endif

#endif
