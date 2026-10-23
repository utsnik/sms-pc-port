#include "input.h"

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum ScriptKind {
    SCRIPT_BUTTON,
    SCRIPT_STICK,
    SCRIPT_POINTER,
};

struct ScriptEvent {
    ScriptKind kind;
    u32 button;
    f32 stick_x;
    f32 stick_y;
    f32 pointer_x;
    f32 pointer_y;
    u32 at;
    u32 until;
    char name[16];
    bool logged;
};

static const u32 kMaxScriptEvents = 96;
ScriptEvent s_script[kMaxScriptEvents];
u32 s_script_count;
bool s_initialized;
bool s_connected;
bool s_marker;
bool s_script_loaded;
u32 s_previous_hold;
u32 s_vpad_hold;
f32 s_vpad_stick_x;
f32 s_vpad_stick_y;
f32 s_gyro_pointer_x;
f32 s_gyro_pointer_y;
f32 s_gyro_center_x;
f32 s_gyro_center_y;
enum PointerSource {
    POINTER_SOURCE_NONE,
    POINTER_SOURCE_TOUCH,
    POINTER_SOURCE_GYRO,
};
PointerSource s_pointer_source;
bool s_hardware_pointer_valid;
f32 s_hardware_pointer_x;
f32 s_hardware_pointer_y;
/* Revolution's Nunchuk+ACC+DPD format is 5; wut names this family
 * WPAD_FMT_NUNCHUK_ACC_DPD only in some SDK revisions. */
u32 s_data_format = 5;
WPADExtensionCallback s_extension_callbacks[WPAD_MAX_CONTROLLERS];
WPADConnectCallback s_connect_callbacks[WPAD_MAX_CONTROLLERS];
WPADSamplingCallback s_sampling_callbacks[WPAD_MAX_CONTROLLERS];
bool s_extension_reported;
bool s_connect_reported;

bool channel_valid(s32 chan)
{
    return chan >= 0 && chan < WPAD_MAX_CONTROLLERS;
}

void trim(char* text)
{
    if (!text)
        return;
    char* first = text;
    while (*first == ' ' || *first == '\t')
        ++first;
    if (first != text)
        std::memmove(text, first, std::strlen(first) + 1);
    size_t length = std::strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t'))
        text[--length] = '\0';
}

bool button_control(const char* name, u32* button)
{
    struct ButtonName { const char* name; u32 button; };
    static const ButtonName controls[] = {
        {"a", WPAD_BUTTON_A}, {"b", WPAD_BUTTON_B},
        {"one", WPAD_BUTTON_1}, {"two", WPAD_BUTTON_2},
        {"plus", WPAD_BUTTON_PLUS}, {"minus", WPAD_BUTTON_MINUS},
        {"home", WPAD_BUTTON_HOME}, {"up", WPAD_BUTTON_UP},
        {"down", WPAD_BUTTON_DOWN}, {"left", WPAD_BUTTON_LEFT},
        {"right", WPAD_BUTTON_RIGHT}, {"c", WPAD_BUTTON_C},
        {"z", WPAD_BUTTON_Z},
    };
    for (const ButtonName& control : controls) {
        if (std::strcmp(control.name, name) == 0) {
            *button = control.button;
            return true;
        }
    }
    return false;
}

bool stick_control(const char* name, f32* x, f32* y)
{
    if (std::strcmp(name, "stick_left") == 0) {
        *x = -1.0f;
        *y = 0.0f;
    } else if (std::strcmp(name, "stick_right") == 0) {
        *x = 1.0f;
        *y = 0.0f;
    } else if (std::strcmp(name, "stick_up") == 0) {
        *x = 0.0f;
        *y = 1.0f;
    } else if (std::strcmp(name, "stick_down") == 0) {
        *x = 0.0f;
        *y = -1.0f;
    } else {
        return false;
    }
    return true;
}

bool parse_uint(const char* text, u32* value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 0xfffffffful)
        return false;
    *value = static_cast<u32>(parsed);
    return true;
}

bool parse_float(const char* text, f32* value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0')
        return false;
    *value = parsed;
    return true;
}

void log_unknown(const char* token)
{
    port_log("[smg-input] autoplay: unknown token '%s'\n", token);
}

bool parse_timing(char* text, u32* at, u32* hold)
{
    char* plus = std::strchr(text, '+');
    if (plus)
        *plus++ = '\0';
    if (!parse_uint(text, at))
        return false;
    *hold = 1;
    if (plus && (!parse_uint(plus, hold) || *hold == 0))
        return false;
    return true;
}

void clamp_pointer(const char* axis, f32* value)
{
    if (*value < -1.0f) {
        port_log("[smg-input] autoplay: pointer %s %f clamped to -1.000000\n", axis,
                 *value);
        *value = -1.0f;
    } else if (*value > 1.0f) {
        port_log("[smg-input] autoplay: pointer %s %f clamped to 1.000000\n", axis,
                 *value);
        *value = 1.0f;
    }
}

void clamp_normalized(f32* value)
{
    if (*value < -1.0f)
        *value = -1.0f;
    else if (*value > 1.0f)
        *value = 1.0f;
}

void set_pointer_source(PointerSource source)
{
    if (source == s_pointer_source)
        return;
    const PointerSource previous = s_pointer_source;
    s_pointer_source = source;
    if (previous == POINTER_SOURCE_NONE)
        return;
    if (source == POINTER_SOURCE_TOUCH)
        port_log("[smg-input] hardware pointer source: touch\n");
    else if (source == POINTER_SOURCE_GYRO)
        port_log("[smg-input] hardware pointer source: gyro\n");
}

void update_hardware_pointer(const VPADStatus& vpad)
{
    VPADTouchData touch;
    std::memset(&touch, 0, sizeof(touch));
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &touch, &vpad.tpNormal);
    if (touch.touched && touch.validity == VPAD_VALID) {
        /* The GamePad touch surface is 854x480, matching the Wii U render
         * target used by the Galaxy port.  KPAD expects -1..1 coordinates. */
        s_hardware_pointer_x = static_cast<f32>(touch.x) / 427.0f - 1.0f;
        s_hardware_pointer_y = static_cast<f32>(touch.y) / 240.0f - 1.0f;
        s_hardware_pointer_valid = true;
        set_pointer_source(POINTER_SOURCE_TOUCH);
        return;
    }

    /* The DRC gyro is exposed as a normalized aiming position relative to
     * the current R-button baseline.  R is intentionally not forwarded as a
     * Wii button: it recentres this fallback pointer. */
    if (vpad.hold & VPAD_BUTTON_R) {
        s_gyro_center_x = vpad.gyro.x;
        s_gyro_center_y = vpad.gyro.y;
        s_gyro_pointer_x = 0.0f;
        s_gyro_pointer_y = 0.0f;
    } else {
        s_gyro_pointer_x = vpad.gyro.x - s_gyro_center_x;
        s_gyro_pointer_y = vpad.gyro.y - s_gyro_center_y;
    }
    clamp_normalized(&s_gyro_pointer_x);
    clamp_normalized(&s_gyro_pointer_y);
    s_hardware_pointer_x = s_gyro_pointer_x;
    s_hardware_pointer_y = s_gyro_pointer_y;
    s_hardware_pointer_valid = true;
    set_pointer_source(POINTER_SOURCE_GYRO);
}

bool append_button_or_stick(char* name, char* timing)
{
    if (s_script_count >= kMaxScriptEvents)
        return false;
    u32 button = 0;
    f32 stick_x = 0.0f;
    f32 stick_y = 0.0f;
    ScriptKind kind;
    if (button_control(name, &button)) {
        kind = SCRIPT_BUTTON;
    } else if (stick_control(name, &stick_x, &stick_y)) {
        kind = SCRIPT_STICK;
    } else {
        return false;
    }
    u32 at = 0;
    u32 hold = 0;
    if (!parse_timing(timing, &at, &hold))
        return false;
    ScriptEvent& event = s_script[s_script_count++];
    event = {};
    event.kind = kind;
    event.button = button;
    event.stick_x = stick_x;
    event.stick_y = stick_y;
    event.at = at;
    event.until = at + hold;
    std::snprintf(event.name, sizeof(event.name), "%s", name);
    return true;
}

bool append_pointer(char* timing)
{
    if (s_script_count >= kMaxScriptEvents)
        return false;
    char* comma = std::strchr(timing, ',');
    if (!comma)
        return false;
    *comma++ = '\0';
    char* second_comma = std::strchr(comma, ',');
    if (!second_comma)
        return false;
    *second_comma++ = '\0';
    char* plus = std::strchr(second_comma, '+');
    if (plus)
        *plus++ = '\0';
    u32 at = 0;
    u32 hold = 1;
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (!parse_uint(timing, &at) || !parse_float(comma, &x) ||
        !parse_float(second_comma, &y) ||
        (plus && (!parse_uint(plus, &hold) || hold == 0)))
        return false;
    clamp_pointer("x", &x);
    clamp_pointer("y", &y);
    ScriptEvent& event = s_script[s_script_count++];
    event = {};
    event.kind = SCRIPT_POINTER;
    event.pointer_x = x;
    event.pointer_y = y;
    event.at = at;
    event.until = at + hold;
    std::snprintf(event.name, sizeof(event.name), "pointer");
    return true;
}

void load_script()
{
    if (s_script_loaded)
        return;
    s_script_loaded = true;

    char marker_path[256];
    std::snprintf(marker_path, sizeof(marker_path), "%sautopress", sms_wiiu_app_root());
    FILE* marker = std::fopen(marker_path, "rb");
    s_marker = marker != nullptr;
    if (marker)
        std::fclose(marker);
    if (!s_marker) {
        s_connected = false;
        port_log("[smg-input] autopress marker absent: port 0 disconnected\n");
        return;
    }

    s_connected = true;
    char script_path[256];
    std::snprintf(script_path, sizeof(script_path), "%ssmg-autoplay", sms_wiiu_app_root());
    FILE* file = std::fopen(script_path, "rb");
    if (file) {
        char buffer[4096];
        size_t length = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        buffer[length] = '\0';
        char* save = nullptr;
        char* token = ::strtok_r(buffer, ",\r\n", &save);
        while (token) {
            trim(token);
            bool advance = true;
            if (*token && *token != '#') {
                if (std::strncmp(token, "pointer@", 8) == 0) {
                    char* x = ::strtok_r(nullptr, ",\r\n", &save);
                    char* y = ::strtok_r(nullptr, ",\r\n", &save);
                    if (!x || std::strchr(x, '@')) {
                        port_log("[smg-input] autoplay: malformed pointer directive '%s'\n",
                                 token);
                        token = x;
                        advance = false;
                    } else {
                        trim(x);
                        if (!y || std::strchr(y, '@')) {
                            port_log("[smg-input] autoplay: malformed pointer directive '%s'\n",
                                     token);
                            token = y;
                            advance = false;
                        } else {
                            trim(y);
                            char pointer_timing[512];
                            if (std::snprintf(pointer_timing, sizeof(pointer_timing),
                                              "%s,%s,%s", token + 8, x, y) >=
                                    static_cast<int>(sizeof(pointer_timing)) ||
                                !append_pointer(pointer_timing))
                                log_unknown(token);
                        }
                    }
                } else {
                    char* at = std::strchr(token, '@');
                    if (!at) {
                        log_unknown(token);
                    } else {
                        *at++ = '\0';
                        trim(token);
                        if (!append_button_or_stick(token, at))
                            log_unknown(token);
                    }
                }
            }
            if (advance)
                token = ::strtok_r(nullptr, ",\r\n", &save);
        }
    }
    port_log("[smg-input] autoplay: %u scripted events\n", (unsigned)s_script_count);
}

void map_vpad(const VPADStatus& vpad)
{
    u32 result = 0;
    if (vpad.hold & VPAD_BUTTON_A) result |= WPAD_BUTTON_A;
    if (vpad.hold & VPAD_BUTTON_B) result |= WPAD_BUTTON_B;
    if (vpad.hold & VPAD_BUTTON_X) result |= WPAD_BUTTON_1;
    if (vpad.hold & VPAD_BUTTON_Y) result |= WPAD_BUTTON_2;
    if (vpad.hold & VPAD_BUTTON_ZR) result |= WPAD_BUTTON_B;
    if (vpad.hold & VPAD_BUTTON_ZL) result |= WPAD_BUTTON_Z;
    if (vpad.hold & VPAD_BUTTON_L) result |= WPAD_BUTTON_C;
    if (vpad.hold & VPAD_BUTTON_LEFT) result |= WPAD_BUTTON_LEFT;
    if (vpad.hold & VPAD_BUTTON_RIGHT) result |= WPAD_BUTTON_RIGHT;
    if (vpad.hold & VPAD_BUTTON_UP) result |= WPAD_BUTTON_UP;
    if (vpad.hold & VPAD_BUTTON_DOWN) result |= WPAD_BUTTON_DOWN;
    if (vpad.hold & VPAD_BUTTON_PLUS) result |= WPAD_BUTTON_PLUS;
    if (vpad.hold & VPAD_BUTTON_MINUS) result |= WPAD_BUTTON_MINUS;
    if (vpad.hold & VPAD_BUTTON_HOME) result |= WPAD_BUTTON_HOME;
    s_vpad_hold = result;
    s_vpad_stick_x = vpad.leftStick.x;
    s_vpad_stick_y = vpad.leftStick.y;
    update_hardware_pointer(vpad);
}

void poll_vpad()
{
    VPADStatus vpad;
    std::memset(&vpad, 0, sizeof(vpad));
    VPADReadError error = VPAD_READ_NO_SAMPLES;
    if (VPADRead(VPAD_CHAN_0, &vpad, 1, &error) > 0 && error == VPAD_READ_SUCCESS) {
        map_vpad(vpad);
        if (!s_marker)
            s_connected = true;
    }
}

void report_callbacks()
{
    if (!s_connected)
        return;
    if (!s_connect_reported) {
        s_connect_reported = true;
        if (s_connect_callbacks[WPAD_CHAN0])
            s_connect_callbacks[WPAD_CHAN0](WPAD_CHAN0, WPAD_ERR_NONE);
    }
    if (!s_extension_reported) {
        s_extension_reported = true;
        if (s_extension_callbacks[WPAD_CHAN0])
            s_extension_callbacks[WPAD_CHAN0](WPAD_CHAN0, WPAD_DEV_FREESTYLE);
    }
}

void fill_status(KPADStatus& status)
{
    std::memset(&status, 0, sizeof(status));
    const u32 field = sms_wiiu_frame_count();
    u32 hold = s_vpad_hold;
    f32 stick_x = s_vpad_stick_x;
    f32 stick_y = s_vpad_stick_y;
    const ScriptEvent* pointer = nullptr;
    for (u32 i = 0; i < s_script_count; ++i) {
        ScriptEvent& event = s_script[i];
        const bool active = field >= event.at && field < event.until;
        if (!active)
            continue;
        if (!event.logged) {
            event.logged = true;
            if (event.kind == SCRIPT_POINTER)
                port_log("[smg-input] autoplay: pointer field %u\n", (unsigned)field);
            else
                port_log("[smg-input] autoplay: press %s field %u\n", event.name,
                         (unsigned)field);
        }
        if (event.kind == SCRIPT_BUTTON)
            hold |= event.button;
        else if (event.kind == SCRIPT_STICK) {
            stick_x = event.stick_x;
            stick_y = event.stick_y;
        } else {
            pointer = &event;
        }
    }

    status.hold = hold;
    status.trig = hold & ~s_previous_hold;
    status.release = s_previous_hold & ~hold;
    s_previous_hold = hold;
    status.acc.x = 0.0f;
    status.acc.y = -1.0f;
    status.acc.z = 0.0f;
    status.acc_value = 1.0f;
    status.pos.x = pointer ? pointer->pointer_x : 0.0f;
    status.pos.y = pointer ? pointer->pointer_y : 0.0f;
    if (!pointer && s_hardware_pointer_valid) {
        status.pos.x = s_hardware_pointer_x;
        status.pos.y = s_hardware_pointer_y;
    }
    status.horizon.x = 1.0f;
    status.horizon.y = 0.0f;
    status.dist = 2.0f;
    status.acc_vertical.x = 1.0f;
    status.acc_vertical.y = 0.0f;
    status.dev_type = WPAD_DEV_FREESTYLE;
    status.wpad_err = WPAD_ERR_NONE;
    status.dpd_valid_fg = (pointer || s_hardware_pointer_valid) ? 2 : 0;
    status.data_format = static_cast<u8>(s_data_format);
    status.ex_status.fs.stick.x = stick_x;
    status.ex_status.fs.stick.y = stick_y;
    status.ex_status.fs.acc.x = 0.0f;
    status.ex_status.fs.acc.y = -1.0f;
    status.ex_status.fs.acc.z = 0.0f;
    status.ex_status.fs.acc_value = 1.0f;
}

} // namespace

extern "C" {

void WPADInit(void)
{
    if (s_initialized)
        return;
    s_initialized = true;
    s_connected = false;
    s_previous_hold = 0;
    s_vpad_hold = 0;
    s_vpad_stick_x = 0.0f;
    s_vpad_stick_y = 0.0f;
    s_gyro_pointer_x = 0.0f;
    s_gyro_pointer_y = 0.0f;
    s_gyro_center_x = 0.0f;
    s_gyro_center_y = 0.0f;
    s_pointer_source = POINTER_SOURCE_NONE;
    s_hardware_pointer_valid = false;
    s_hardware_pointer_x = 0.0f;
    s_hardware_pointer_y = 0.0f;
    std::memset(s_extension_callbacks, 0, sizeof(s_extension_callbacks));
    std::memset(s_connect_callbacks, 0, sizeof(s_connect_callbacks));
    std::memset(s_sampling_callbacks, 0, sizeof(s_sampling_callbacks));
    load_script();
}

void KPADInit(void)
{
    WPADInit();
    VPADInit();
}

void KPADReset(void)
{
    s_previous_hold = 0;
    s_connect_reported = false;
    s_extension_reported = false;
}

void KPADSetAccParam(s32, f32, f32) {}
void KPADSetBtnRepeat(s32, f32, f32) {}
void KPADSetDistParam(s32, f32, f32) {}
void KPADSetHoriParam(s32, f32, f32) {}
void KPADSetPosParam(s32, f32, f32) {}
void KPADSetSensorHeight(s32, f32) {}

uint32_t KPADReadEx(s32 chan, KPADStatus* statuses, uint32_t count, s32* error)
{
    if (error)
        *error = KPAD_ERROR_NO_SAMPLES;
    if (!channel_valid(chan) || chan != WPAD_CHAN0 || !statuses || !count)
        return 0;
    poll_vpad();
    report_callbacks();
    if (!s_connected)
        return 0;
    fill_status(statuses[0]);
    if (error)
        *error = KPAD_ERROR_OK;
    return 1;
}

s32 KPADRead(s32 chan, KPADStatus* statuses, uint32_t count)
{
    s32 error = KPAD_ERROR_NO_SAMPLES;
    return KPADReadEx(chan, statuses, count, &error);
}

WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback callback)
{
    if (!channel_valid(chan))
        return nullptr;
    WPADExtensionCallback old = s_extension_callbacks[chan];
    s_extension_callbacks[chan] = callback;
    if (chan == WPAD_CHAN0 && callback && s_connected && !s_extension_reported) {
        s_extension_reported = true;
        callback(chan, WPAD_DEV_FREESTYLE);
    }
    return old;
}

WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback callback)
{
    if (!channel_valid(chan))
        return nullptr;
    WPADConnectCallback old = s_connect_callbacks[chan];
    s_connect_callbacks[chan] = callback;
    if (chan == WPAD_CHAN0 && callback && s_connected && !s_connect_reported) {
        s_connect_reported = true;
        callback(chan, WPAD_ERR_NONE);
    }
    return old;
}

WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback callback)
{
    if (!channel_valid(chan))
        return nullptr;
    WPADSamplingCallback old = s_sampling_callbacks[chan];
    s_sampling_callbacks[chan] = callback;
    return old;
}

s32 WPADProbe(s32 chan, u32* type)
{
    if (chan == WPAD_CHAN0 && s_connected) {
        if (type)
            *type = WPAD_DEV_FREESTYLE;
        return WPAD_ERR_NONE;
    }
    if (type)
        *type = WPAD_DEV_NOT_FOUND;
    return WPAD_ERR_NO_CONTROLLER;
}

s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback callback)
{
    if (info) {
        std::memset(info, 0, sizeof(*info));
        info->battery = WPAD_BATTERY_LEVEL_MAX;
        info->led = 1;
    }
    if (callback)
        callback(chan, s_connected ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER);
    return WPAD_ERR_NONE;
}

void WPADControlMotor(s32, u32) {}
BOOL WPADIsSpeakerEnabled(s32 chan) { return chan == WPAD_CHAN0 && s_connected; }
s32 WPADControlSpeaker(s32 chan, u32, WPADCallback callback)
{
    if (callback)
        callback(chan, WPAD_ERR_NONE);
    return WPAD_ERR_NONE;
}
u8 WPADGetSpeakerVolume(void) { return 0x60; }
BOOL WPADCanSendStreamData(s32 chan) { return chan == WPAD_CHAN0 && s_connected; }
s32 WPADSendStreamData(s32, void*, u16) { return WPAD_ERR_NONE; }
void WPADDisconnect(s32) {}
u32 WPADGetWorkMemorySize(void) { return 0x10000; }
void WPADRegisterAllocator(WPADAlloc, WPADFree) {}
void WPADSetAutoSleepTime(u8) {}
u8 WPADGetSensorBarPosition(void) { return WPAD_SENSOR_BAR_POS_TOP; }
u8 WPADGetDpdSensitivity(void) { return 0; }
void WPADGetAccGravityUnit(s32, u32, WPADAcc* acc)
{
    if (acc) {
        acc->x = 0;
        acc->y = -256;
        acc->z = 0;
    }
}
BOOL WPADStopSimpleSync(void) { return 1; }
BOOL WPADCancelSyncDevice(void) { return 1; }
BOOL WPADIsDpdEnabled(s32 chan) { return chan == WPAD_CHAN0 && s_connected; }
s32 WPADControlDpd(s32 chan, u32, WPADCallback callback)
{
    if (callback)
        callback(chan, WPAD_ERR_NONE);
    return WPAD_ERR_NONE;
}
u32 WPADGetDataFormat(s32) { return s_data_format; }
s32 WPADSetDataFormat(s32, u32 format)
{
    s_data_format = format;
    return WPAD_ERR_NONE;
}
void WPADRead(s32, void*) {}
s32 WPADGetStatus(void) { return WPAD_ERR_NONE; }
void WPADSetAutoSamplingBuf(s32, void*, u32) {}
s32 WPADControlLed(s32 chan, u8, WPADCallback callback)
{
    if (callback)
        callback(chan, WPAD_ERR_NONE);
    return WPAD_ERR_NONE;
}
void WPADGetAddress(s32, u8* address)
{
    if (address)
        std::memset(address, 0, 6);
}

} // extern "C"
