#include "runtime.h"

#include <dolphin/pad.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>

#include <cstdint>
#include <cstring>

namespace {

PADStatus s_status[PAD_MAX_CONTROLLERS];
bool s_initialized;
bool s_first_success;
int s_last_error[PAD_MAX_CONTROLLERS];
uint32_t s_connected_mask;

/* wut sticks and GameCube PADStatus are both up-positive; PADClamp expects
 * about +/-100 at full deflection, as the PC port feeds it. */
s8 stick_value(float value)
{
	if (value < -1.0f)
		value = -1.0f;
	if (value > 1.0f)
		value = 1.0f;
	return static_cast<s8>(value * 100.0f);
}

u16 map_vpad_buttons(uint32_t buttons)
{
	u16 result = 0;
	if (buttons & VPAD_BUTTON_LEFT) result |= PAD_BUTTON_LEFT;
	if (buttons & VPAD_BUTTON_RIGHT) result |= PAD_BUTTON_RIGHT;
	if (buttons & VPAD_BUTTON_DOWN) result |= PAD_BUTTON_DOWN;
	if (buttons & VPAD_BUTTON_UP) result |= PAD_BUTTON_UP;
	if (buttons & VPAD_BUTTON_A) result |= PAD_BUTTON_A;
	if (buttons & VPAD_BUTTON_B) result |= PAD_BUTTON_B;
	if (buttons & VPAD_BUTTON_X) result |= PAD_BUTTON_X;
	if (buttons & VPAD_BUTTON_Y) result |= PAD_BUTTON_Y;
	if (buttons & VPAD_BUTTON_PLUS) result |= PAD_BUTTON_START;
	if (buttons & VPAD_BUTTON_ZL) result |= PAD_TRIGGER_Z;
	if (buttons & VPAD_BUTTON_ZR) result |= PAD_TRIGGER_Z;
	if (buttons & VPAD_BUTTON_L) result |= PAD_TRIGGER_L;
	if (buttons & VPAD_BUTTON_R) result |= PAD_TRIGGER_R;
	return result;
}

u16 map_kpad_buttons(uint32_t buttons)
{
	u16 result = 0;
	if (buttons & WPAD_BUTTON_LEFT) result |= PAD_BUTTON_LEFT;
	if (buttons & WPAD_BUTTON_RIGHT) result |= PAD_BUTTON_RIGHT;
	if (buttons & WPAD_BUTTON_DOWN) result |= PAD_BUTTON_DOWN;
	if (buttons & WPAD_BUTTON_UP) result |= PAD_BUTTON_UP;
	if (buttons & WPAD_BUTTON_A) result |= PAD_BUTTON_A;
	if (buttons & WPAD_BUTTON_B) result |= PAD_BUTTON_B;
	if (buttons & WPAD_BUTTON_1) result |= PAD_BUTTON_X;
	if (buttons & WPAD_BUTTON_2) result |= PAD_BUTTON_Y;
	if (buttons & WPAD_BUTTON_PLUS) result |= PAD_BUTTON_START;
	if (buttons & WPAD_BUTTON_Z) result |= PAD_TRIGGER_Z;
	if (buttons & WPAD_BUTTON_C) result |= PAD_TRIGGER_Z;
	return result;
}

/* Classic and Pro Controller buttons arrive in the extension's own hold word. */
u16 map_ext_buttons(uint32_t b)
{
	u16 result = 0;
	if (b & WPAD_PRO_BUTTON_LEFT) result |= PAD_BUTTON_LEFT;
	if (b & WPAD_PRO_BUTTON_RIGHT) result |= PAD_BUTTON_RIGHT;
	if (b & WPAD_PRO_BUTTON_DOWN) result |= PAD_BUTTON_DOWN;
	if (b & WPAD_PRO_BUTTON_UP) result |= PAD_BUTTON_UP;
	if (b & WPAD_PRO_BUTTON_A) result |= PAD_BUTTON_A;
	if (b & WPAD_PRO_BUTTON_B) result |= PAD_BUTTON_B;
	if (b & WPAD_PRO_BUTTON_X) result |= PAD_BUTTON_X;
	if (b & WPAD_PRO_BUTTON_Y) result |= PAD_BUTTON_Y;
	if (b & WPAD_PRO_BUTTON_PLUS) result |= PAD_BUTTON_START;
	if (b & (WPAD_PRO_BUTTON_ZL | WPAD_PRO_BUTTON_ZR)) result |= PAD_TRIGGER_Z;
	if (b & WPAD_PRO_BUTTON_L) result |= PAD_TRIGGER_L;
	if (b & WPAD_PRO_BUTTON_R) result |= PAD_TRIGGER_R;
	return result;
}

/* Fill one GameCube pad from a Wii Remote channel; false if nothing new or
 * not connected. */
bool read_kpad(unsigned kchan, PADStatus& out)
{
	KPADStatus kpad;
	std::memset(&kpad, 0, sizeof(kpad));
	KPADError error = KPAD_ERROR_NO_SAMPLES;
	uint32_t count = KPADReadEx(static_cast<KPADChan>(kchan), &kpad, 1, &error);
	if (!count || error != KPAD_ERROR_OK)
		return false;
	std::memset(&out, 0, sizeof(out));
	if (kpad.extensionType == WPAD_EXT_PRO_CONTROLLER) {
		out.button = map_ext_buttons(kpad.pro.hold);
		out.stickX = stick_value(kpad.pro.leftStick.x);
		out.stickY = stick_value(kpad.pro.leftStick.y);
		out.substickX = stick_value(kpad.pro.rightStick.x);
		out.substickY = stick_value(kpad.pro.rightStick.y);
		out.triggerLeft = (kpad.pro.hold & WPAD_PRO_BUTTON_L) ? 0xff : 0;
		out.triggerRight = (kpad.pro.hold & WPAD_PRO_BUTTON_R) ? 0xff : 0;
	} else if (kpad.extensionType == WPAD_EXT_CLASSIC) {
		out.button = map_ext_buttons(kpad.classic.hold);
		out.stickX = stick_value(kpad.classic.leftStick.x);
		out.stickY = stick_value(kpad.classic.leftStick.y);
		out.substickX = stick_value(kpad.classic.rightStick.x);
		out.substickY = stick_value(kpad.classic.rightStick.y);
		out.triggerLeft = static_cast<u8>(kpad.classic.leftTrigger * 0xff);
		out.triggerRight = static_cast<u8>(kpad.classic.rightTrigger * 0xff);
	} else {
		out.button = map_kpad_buttons(kpad.hold);
		if (kpad.extensionType == WPAD_EXT_NUNCHUK) {
			out.stickX = stick_value(kpad.nunchuk.stick.x);
			out.stickY = stick_value(kpad.nunchuk.stick.y);
		}
	}
	out.err = PAD_ERR_NONE;
	return true;
}

void log_error_transition(unsigned chan, int error)
{
	if (s_last_error[chan] == error)
		return;
	s_last_error[chan] = error;
	port_log("[pad] controller channel=%u error=%d\n", chan, error);
}

} // namespace

extern "C" {

void port_pad_init(void)
{
	if (s_initialized)
		return;
	s_initialized = true;
	std::memset(s_status, 0, sizeof(s_status));
	for (unsigned i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
		s_status[i].err = PAD_ERR_NO_CONTROLLER;
		s_last_error[i] = 0x7fffffff;
	}
	VPADInit();
	KPADInit();
	port_log("[pad] VPAD/KPAD input ready; neutral state is stable without a controller\n");
}

BOOL PADInit(void)
{
	port_pad_init();
	return TRUE;
}

/* The only hardware reader: once per frame at the main-thread frame boundary.
 * PADRead hands the game this cached state, so a second read in the same frame
 * cannot see "no new samples" and drop the controller or a press. Port 0 is the
 * GamePad, or Wii Remote 1 when no GamePad sample arrives; ports 1-3 are
 * Wii Remotes 2-4. A channel with no new sample keeps its last state. */
void sms_wiiu_input_poll(void)
{
	if (!s_initialized)
		return;
	uint32_t connected = 0;
	VPADStatus vpad;
	std::memset(&vpad, 0, sizeof(vpad));
	VPADReadError vpad_error = VPAD_READ_NO_SAMPLES;
	int32_t vpad_count = VPADRead(static_cast<VPADChan>(0), &vpad, 1, &vpad_error);
	PADStatus kpad0;
	bool have_kpad0 = read_kpad(0, kpad0);
	if (vpad_count > 0 && vpad_error == VPAD_READ_SUCCESS) {
		PADStatus& out = s_status[0];
		std::memset(&out, 0, sizeof(out));
		out.button = map_vpad_buttons(vpad.hold);
		out.stickX = stick_value(vpad.leftStick.x);
		out.stickY = stick_value(vpad.leftStick.y);
		out.substickX = stick_value(vpad.rightStick.x);
		out.substickY = stick_value(vpad.rightStick.y);
		out.triggerLeft = (vpad.hold & VPAD_BUTTON_L) ? 0xff : 0;
		out.triggerRight = (vpad.hold & VPAD_BUTTON_R) ? 0xff : 0;
		if (have_kpad0)
			out.button |= kpad0.button;
		out.err = PAD_ERR_NONE;
		if (!s_first_success) {
			s_first_success = true;
			port_log("[pad] first successful GamePad poll\n");
		}
	} else if (have_kpad0) {
		s_status[0] = kpad0;
	} else if (vpad_error != VPAD_READ_NO_SAMPLES) {
		std::memset(&s_status[0], 0, sizeof(PADStatus));
		s_status[0].err = PAD_ERR_NO_CONTROLLER;
	}
	for (unsigned chan = 1; chan < PAD_MAX_CONTROLLERS; ++chan) {
		PADStatus remote;
		if (read_kpad(chan, remote)) {
			s_status[chan] = remote;
		} else {
			WPADExtensionType type;
			if (WPADProbe(static_cast<WPADChan>(chan), &type) != 0) {
				std::memset(&s_status[chan], 0, sizeof(PADStatus));
				s_status[chan].err = PAD_ERR_NO_CONTROLLER;
			}
		}
	}
	for (unsigned chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan) {
		if (s_status[chan].err == PAD_ERR_NONE)
			connected |= 1u << chan;
		log_error_transition(chan, s_status[chan].err);
	}
	if (connected != s_connected_mask)
		port_log("[pad] connected-channel-mask=0x%x\n", connected);
	s_connected_mask = connected;
}

u32 PADRead(PADStatus* output)
{
	if (!output)
		return 0;
	std::memcpy(output, s_status, sizeof(s_status));
	return s_connected_mask;
}

int PADReset(unsigned long mask)
{
	(void)mask;
	return 1;
}
BOOL PADRecalibrate(u32 mask)
{
	(void)mask;
	return TRUE;
}
void PADControlMotor(s32 chan, u32 command)
{
	static unsigned s_rumble_logged;
	if (s_rumble_logged < 4) {
		++s_rumble_logged;
		port_log("[pad] rumble no-op channel=%d command=%u\n", chan, command);
	}
}
void PADControlAllMotors(const u32*) {}
void PADSetSamplingRate(unsigned long) {}
void __PADTestSamplingRate(unsigned long) {}
void PADSetSpec(u32) {}
unsigned long PADGetSpec(void) { return PAD_SPEC_5; }
int PADGetType(long, unsigned long* type)
{
	if (type)
		*type = 0;
	return 0;
}
BOOL PADSync(void) { return TRUE; }
void PADSetAnalogMode(u32) {}

int port_camera_take_mouse(f32* dx, f32* dy)
{
	if (dx)
		*dx = 0.0f;
	if (dy)
		*dy = 0.0f;
	return 0;
}

} // extern "C"
