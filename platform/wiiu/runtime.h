#ifndef SMS_PLATFORM_WIIU_RUNTIME_H
#define SMS_PLATFORM_WIIU_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU-only d4 GPU submission breadcrumbs. */
enum D4Event {
	D4_DRAW, D4_CLEAR_COLOR, D4_CLEAR_DEPTH, D4_EFB_COPY, D4_DEPTH_COPY,
	D4_RETile, D4_SCAN_COPY, D4_PEEK, D4_UPLOAD, D4_SWAP, D4_CONTEXT,
	D4_TARGET, D4_READBACK
};
void sms_wiiu_d4_event(uint32_t event);
void sms_wiiu_d4_draw(uint32_t verts, uint32_t stages, uint32_t blend, uint32_t texmask);
void sms_wiiu_d4_submit(const char* kind, uint64_t raw_timestamp);
void sms_wiiu_d4_submit_at(const char* kind, char file_char, uint32_t line,
                           uint64_t raw_timestamp);
void sms_wiiu_d4_timeout(uint64_t ret_after);

/* The active PORT_GAME runtime owns the target MEM1/MEM2 bounds.  SMG uses
 * static Wii-sized arenas; SMS keeps its default-heap runtime ownership. */
void sms_wiiu_runtime_init(void);
void sms_wiiu_sdl_audio_preopen(int rate);

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
void sms_wiiu_dvd_shutdown(void);
int port_dvd_busy(void);
const char* port_dvd_stage_path(void);
void sms_wiiu_gfx_init(void);
void sms_wiiu_d1_note_copy_xfb_triple(void);
void sms_wiiu_d1_note_vi_flush(void);
void port_pad_init(void);
void* sms_wiiu_dvd_read_file(const char* path, size_t* size);
int64_t sms_wiiu_OSGetTime(void);
#if defined(PORT_GAME_SMG)
void sms_wiiu_frame_split_begin(void);
void sms_wiiu_frame_split_end(u32 fields);
void sms_wiiu_frame_split_gx_begin(void);
void sms_wiiu_frame_split_gx_end(void);
void sms_wiiu_frame_split_gpu_begin(void);
void sms_wiiu_frame_split_gpu_end(void);
void sms_wiiu_frame_split_audio_begin(void);
void sms_wiiu_frame_split_audio_end(void);
#endif

void port_init(int argc, char** argv);
void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void* port_low_alloc(unsigned long size);
void port_ptr32_trap(const void* ptr);
int64_t port_time_ticks(void);

#ifdef __cplusplus
}
#endif

#endif
