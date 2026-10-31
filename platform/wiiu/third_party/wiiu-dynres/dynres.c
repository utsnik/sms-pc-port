#include "dynres.h"
#include <string.h>

void dynres_default_config(dynres_config* cfg, const uint16_t* lines, uint8_t n_lines, uint32_t target_us) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->target_us = target_us;
    cfg->lines = lines;
    cfg->n_lines = n_lines;
    cfg->window_frames = 30;
    cfg->down_permille = 930;
    cfg->down_windows = 3;
    cfg->cooldown_windows = 2;
    cfg->up_permille = 850;
    cfg->min_up_windows = 20;
}

int dynres_init(dynres_ctx* ctx, const dynres_config* cfg) {
    memset(ctx, 0, sizeof(*ctx));
    if (!cfg->lines || cfg->n_lines == 0 || cfg->n_lines > DYNRES_MAX_SIZES || !cfg->target_us ||
        !cfg->window_frames || !cfg->down_windows || cfg->down_windows > DYNRES_MAX_DOWN_WINDOWS)
        return -1;
    for (uint8_t i = 0; i < cfg->n_lines; ++i) {
        if (!cfg->lines[i] || (i && cfg->lines[i] >= cfg->lines[i - 1])) return -1;
        ctx->lines[i] = cfg->lines[i];
    }
    ctx->cfg = *cfg;
    ctx->cfg.lines = ctx->lines;
    return 0;
}

static int index_of(const dynres_ctx* ctx, int lines) {
    for (uint8_t i = 0; i < ctx->cfg.n_lines; ++i)
        if (ctx->lines[i] == lines) return i;
    return -1;
}

uint8_t dynres_begin_stage(dynres_ctx* ctx, int remembered_lines, int default_lines) {
    int idx = index_of(ctx, remembered_lines);
    if (idx < 0) idx = index_of(ctx, default_lines);
    if (idx < 0) idx = 0;
    ctx->index = (uint8_t)idx;
    ctx->active = 1;
    ctx->stepped_down = 0;
    ctx->over = 0;
    ctx->cooldown = ctx->cfg.cooldown_windows;  /* the stage start (load, streaming) is not judged */
    ctx->frames = 0;
    ctx->sum_us = 0;
    ctx->measured = 0;
    ctx->heaviest_us = 0;
    ctx->recent_n = ctx->recent_pos = 0;
    return ctx->index;
}

uint8_t dynres_frame(dynres_ctx* ctx, uint32_t gpu_busy_us) {
    if (!ctx->active) return ctx->index;
    ctx->sum_us += gpu_busy_us;
    if (++ctx->frames < ctx->cfg.window_frames) return ctx->index;
    const uint32_t avg = (uint32_t)(ctx->sum_us / ctx->frames);
    ctx->frames = 0;
    ctx->sum_us = 0;
    ctx->last_avg_us = avg;
    if (ctx->cooldown) {
        --ctx->cooldown;
        ctx->over = 0;
        return ctx->index;
    }
    ++ctx->measured;
    /* Sustained load: a short burst (a stage's first textures and shaders)
     * neither steps down nor blocks a later step up. */
    ctx->recent[ctx->recent_pos] = avg;
    ctx->recent_pos = (uint8_t)((ctx->recent_pos + 1) % ctx->cfg.down_windows);
    if (ctx->recent_n < ctx->cfg.down_windows) ++ctx->recent_n;
    if (ctx->recent_n == ctx->cfg.down_windows) {
        uint32_t sustained = ctx->recent[0];
        for (uint8_t i = 1; i < ctx->recent_n; ++i)
            if (ctx->recent[i] < sustained) sustained = ctx->recent[i];
        if (sustained > ctx->heaviest_us) ctx->heaviest_us = sustained;
    }
    const uint64_t limit = (uint64_t)ctx->cfg.target_us * ctx->cfg.down_permille / 1000u;
    if (avg <= limit) {
        ctx->over = 0;
        return ctx->index;
    }
    if (++ctx->over >= ctx->cfg.down_windows && ctx->index + 1 < ctx->cfg.n_lines) {
        ++ctx->index;
        ctx->stepped_down = 1;
        ctx->over = 0;
        ctx->cooldown = ctx->cfg.cooldown_windows;
        ctx->recent_n = ctx->recent_pos = 0;
    }
    return ctx->index;
}

int dynres_end_stage(dynres_ctx* ctx) {
    if (!ctx->active) return 0;
    ctx->active = 0;
    const int cur = ctx->lines[ctx->index];
    if (ctx->stepped_down) return cur;
    if (ctx->measured < ctx->cfg.min_up_windows) return 0;
    if (ctx->index == 0) return cur;
    const uint64_t big = ctx->lines[ctx->index - 1];
    const uint64_t limit = (uint64_t)ctx->cfg.target_us * ctx->cfg.up_permille / 1000u;
    /* Conservative: assume all GPU time grows with the pixel count. */
    if ((uint64_t)ctx->heaviest_us * big * big <= limit * (uint64_t)cur * (uint64_t)cur) return (int)big;
    return cur;
}

uint16_t dynres_lines(const dynres_ctx* ctx) { return ctx->lines[ctx->index]; }
