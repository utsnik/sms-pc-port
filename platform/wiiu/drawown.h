#pragma once

#if defined(PORT_GAME_SMG)

enum SmsWiiUDrawownKind {
    SMS_WIIU_DRAWOWN_J3D = 0,
    SMS_WIIU_DRAWOWN_JPA = 1,
    SMS_WIIU_DRAWOWN_2D = 2,
    SMS_WIIU_DRAWOWN_OTHER = 3,
};

#ifdef __cplusplus
extern "C" {
#endif
void sms_wiiu_drawown_push_name(const char* name, const void* object);
void sms_wiiu_drawown_push_draw_type(unsigned draw_type);
void sms_wiiu_drawown_push_kind(unsigned kind);
void sms_wiiu_drawown_pop(void);
void sms_wiiu_drawown_record(unsigned verts);
#ifdef __cplusplus
}
#endif

#endif
