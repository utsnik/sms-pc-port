#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void sms_wiiu_jpa_particle(void);
void sms_wiiu_jpa_draw(void);
void sms_wiiu_jpa_split(void);
void sms_wiiu_jpa_batched(void);
void sms_wiiu_jpa_reject_type(void);
void sms_wiiu_jpa_reject_callback(void);
void sms_wiiu_jpa_reject_prj(void);
void sms_wiiu_jpa_reject_ineligible(void);
void sms_wiiu_jpa_type3(void);
void sms_wiiu_jpa_type4(void);
void sms_wiiu_jpa_histogram(int child, unsigned type, const char* func, int callback,
                            const void* emitter);
#ifdef __cplusplus
}
#endif
