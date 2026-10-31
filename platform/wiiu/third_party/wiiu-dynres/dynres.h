/* wiiu-dynres: render-size controller driven by measured GPU busy time.
 * Pure C11, no heap, no platform calls.  During a stage visit the size only
 * steps down; a step up is decided once, when the visit ends. */
#ifndef WIIU_DYNRES_H
#define WIIU_DYNRES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DYNRES_MAX_SIZES 8
#define DYNRES_MAX_DOWN_WINDOWS 8

typedef struct {
    uint32_t target_us;        /* frame budget, e.g. 33333 */
    const uint16_t* lines;     /* sizes, largest first, e.g. {1080, 900, 810} */
    uint8_t n_lines;
    uint8_t window_frames;     /* frames per averaging window (30) */
    uint16_t down_permille;    /* step down above this share of target (930) */
    uint8_t down_windows;      /* consecutive windows over the limit (3); also the length
                                  of the stretch that counts as sustained load */
    uint8_t cooldown_windows;  /* windows ignored after a begin or a step down (2) */
    uint16_t up_permille;      /* end-of-visit fit margin (850) */
    uint8_t min_up_windows;    /* measured windows a visit needs to earn a step up (20) */
} dynres_config;

typedef struct {
    dynres_config cfg;
    uint16_t lines[DYNRES_MAX_SIZES];
    uint8_t index;          /* current size */
    uint8_t active;         /* a visit is open */
    uint8_t stepped_down;   /* this visit stepped down */
    uint8_t over;           /* consecutive windows over the limit */
    uint8_t cooldown;       /* windows still ignored */
    uint32_t frames;        /* frames in the current window */
    uint64_t sum_us;        /* busy time in the current window */
    uint32_t measured;      /* windows measured this visit (outside cooldown) */
    uint32_t heaviest_us;   /* heaviest sustained load this visit: the largest minimum
                               over down_windows consecutive measured windows */
    uint32_t recent[DYNRES_MAX_DOWN_WINDOWS];  /* the last measured window averages */
    uint8_t recent_n, recent_pos;
    uint8_t held;           /* the current window is not judged (dynres_hold) */
    uint32_t last_avg_us;   /* the last complete window's average */
} dynres_ctx;

void dynres_default_config(dynres_config* cfg, const uint16_t* lines, uint8_t n_lines, uint32_t target_us);
int dynres_init(dynres_ctx* ctx, const dynres_config* cfg);
uint8_t dynres_begin_stage(dynres_ctx* ctx, int remembered_lines, int default_lines);
uint8_t dynres_frame(dynres_ctx* ctx, uint32_t gpu_busy_us);
/* The current window is not judged: neither a step down, nor the heaviest
 * load, nor a break in a run of slow windows.  For load the render size
 * cannot fix, such as textures streaming in at a stage's start. */
void dynres_hold(dynres_ctx* ctx);
int dynres_end_stage(dynres_ctx* ctx);
uint16_t dynres_lines(const dynres_ctx* ctx);

#ifdef __cplusplus
}
#endif
#endif
