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

/* Activates only the neutral lower GX boundary; no renderer is started. */
void smg_wiiu_gx_link_stub_init(void);

#ifdef __cplusplus
}
#endif
