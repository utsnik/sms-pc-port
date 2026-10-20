#ifndef SMS_PLATFORM_WIIU_RUNTIME_H
#define SMS_PLATFORM_WIIU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 2 owns an aligned MEM2 arena obtained from the Wii U default heap.
 * MEM1 is queried for the GameCube compatibility boundary but is not used as
 * the game heap because foreground/system services own most of it. */
void sms_wiiu_runtime_init(void);

extern u8* port_mem1_base;
extern u32 port_mem1_size;
extern u8* sms_wiiu_mem2_base;
extern u32 sms_wiiu_mem2_size;

const char* sms_wiiu_app_root(void);
/* 1 when the file <app root>/<name> exists (read-only SD switches). */
int sms_wiiu_sd_marker(const char* name);
uint32_t sms_wiiu_heap_free(void);
u32 sms_wiiu_frame_count(void);
const char* sms_wiiu_stage_path(void);
void sms_wiiu_frame_boundary(void);
void sms_wiiu_input_poll(void);
void sms_wiiu_dvd_pump(void);
int port_dvd_busy(void);
const char* port_dvd_stage_path(void);
void sms_wiiu_gfx_init(void);
void port_pad_init(void);
void* sms_wiiu_dvd_read_file(const char* path, size_t* size);
int64_t sms_wiiu_OSGetTime(void);

void port_init(int argc, char** argv);
void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void* port_low_alloc(unsigned long size);
void port_ptr32_trap(const void* ptr);
int64_t port_time_ticks(void);

#ifdef __cplusplus
}
#endif

#endif
