#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage 1 retains the game entry but never transfers control to it. */
void smg_wiiu_runtime_init(void);

/* Fixed target-owned storage for link-only port allocations. */
void* smg_wiiu_arena_alloc(size_t size, size_t alignment);
uint32_t smg_wiiu_arena_used(void);
extern uint8_t* port_mem1_base;
extern uint32_t port_mem1_size;
extern uint8_t* sms_wiiu_mem2_base;
extern uint32_t sms_wiiu_mem2_size;

/* Activates only the neutral lower GX boundary; no renderer is started. */
void smg_wiiu_gx_link_stub_init(void);

/* Task 2's retail FST/DVD boundary, used by the native-BE main.dol loader. */
void port_dvd_init(void);
void* sms_wiiu_dvd_read_file(const char* path, size_t* size);

/* Cemu's per-title save directory, used as the NAND provider's host root. */
const char* smg_wiiu_save_root(void);
const char* sms_wiiu_app_root(void);
void port_log(const char* fmt, ...);

/* The shared VI boundary counts presented fields; Galaxy autoplay is keyed
 * to that same deterministic counter. */
uint32_t sms_wiiu_frame_count(void);

#ifdef __cplusplus
}
#endif
