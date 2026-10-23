#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the Galaxy-owned Wii U runtime boundary and app root. */
void smg_wiiu_runtime_init(void);

extern uint8_t* port_mem1_base;
extern uint32_t port_mem1_size;
extern uint8_t* sms_wiiu_mem2_base;
extern uint32_t sms_wiiu_mem2_size;
/* A MEM2 address (DsetVARAM base + ARAM offset) as memory; NULL outside MEM2. */
const uint8_t* port_varam_ptr(uint32_t addr);
void smg_wiiu_bind_varam_base(uint32_t base, uint32_t size);
/* Called immediately after Galaxy creates JKRHeap::sRootHeap. */
void smg_wiiu_game_heap_ready(void);

/* Task 2's retail FST/DVD boundary, used by the native-BE main.dol loader. */
void port_dvd_init(void);
void* sms_wiiu_dvd_read_file(const char* path, size_t* size);

/* Shared Wii U services owned by the SMG entry sequence. */
void port_os_init(void);
void port_os_threads_init(void);
void port_vi_init(void);
void smg_wiiu_gx_link_stub_init(void);
void port_pad_init(void);

/* Opens the target audio output before JAudio's own initializer creates its
 * DVD and audio threads. The pacer is started exactly once by JASAudioThread. */
void sms_wiiu_sdl_audio_preopen(int rate);

/* Opens AX/SDL and reserves the VARAM window before Galaxy JAudio2 starts. */
void smg_wiiu_audio_init(void);

/* Task 3's native-Espresso-BE data loader. */
void smg_wiiu_load_dol_data(void);

/* Cemu's per-title save directory, used as the NAND provider's host root. */
const char* smg_wiiu_save_root(void);
const char* sms_wiiu_app_root(void);
int sms_wiiu_sd_marker(const char* name);
void port_log(const char* fmt, ...);

/* The shared VI boundary counts presented fields; Galaxy autoplay is keyed
 * to that same deterministic counter. */
uint32_t sms_wiiu_frame_count(void);

#ifdef __cplusplus
}
#endif
