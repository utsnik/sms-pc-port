#ifndef SMS_WIIU_GRASS_H
#define SMS_WIIU_GRASS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw ABI only: the game patch never includes GX2, JGeometry, or a renderer
 * owned type.  PC preprocessing omits every call site in the patch. */
void sms_wiiu_grass_scene_begin(void);
/* width/swing_width are TMapObjGrassManager::mWidth/mSwingWidth: they bound
 * the far s16 sums (a group that could wrap s16 keeps the CPU loop). */
void sms_wiiu_grass_register_group(const void* group, uint32_t count,
                                   const void* near_positions,
                                   const void* far_base_positions,
                                   const void* far_apex_positions,
                                   float base_y, float width,
                                   float swing_width);
int sms_wiiu_grass_draw_near(const void* group, const void* draw_vec,
                             const float* swings);
int sms_wiiu_grass_draw_far(const void* group, const void* draw_vec_s16,
                            const int16_t* swings);
int sms_wiiu_grass_cpu_fallback(void);

#ifdef __cplusplus
}
#endif

#endif
