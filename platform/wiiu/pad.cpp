#include "runtime.h"

#include <dolphin/pad.h>
#include <padscore/kpad.h>
#include <vpad/input.h>

#include <cstdint>
#include <cstring>

namespace {

PADStatus s_status[PAD_MAX_CONTROLLERS];
bool s_initialized;
bool s_first_success;
int s_last_error[PAD_MAX_CONTROLLERS];
uint32_t s_connected_mask;

s8 stick_value(float value)
{
	if (value < -1.0f)
		value = -1.0f;
	if (value > 1.0f)
		value = 1.0f;
	return static_cast<s8>(value * 127.0f);
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

void sms_wiiu_input_poll(void)
{
	if (!s_initialized)
		return;
	PADRead(s_status);
}

u32 PADRead(PADStatus* output)
{
	if (!output)
		return 0;
	for (unsigned i = 0; i < PAD_MAX_CONTROLLERS; ++i) {
		std::memset(&output[i], 0, sizeof(PADStatus));
		output[i].err = PAD_ERR_NO_CONTROLLER;
	}
	uint32_t connected = 0;
	VPADStatus vpad;
	std::memset(&vpad, 0, sizeof(vpad));
	VPADReadError vpad_error = VPAD_READ_NO_SAMPLES;
	int32_t vpad_count = VPADRead(static_cast<VPADChan>(0), &vpad, 1, &vpad_error);
	if (vpad_count > 0 && vpad_error == VPAD_READ_SUCCESS) {
		output[0].button = map_vpad_buttons(vpad.hold);
		output[0].stickX = stick_value(vpad.leftStick.x);
		output[0].stickY = stick_value(-vpad.leftStick.y);
		output[0].substickX = stick_value(vpad.rightStick.x);
		output[0].substickY = stick_value(-vpad.rightStick.y);
		output[0].triggerLeft = (vpad.hold & VPAD_BUTTON_L) ? 0xff : 0;
		output[0].triggerRight = (vpad.hold & VPAD_BUTTON_R) ? 0xff : 0;
		output[0].err = PAD_ERR_NONE;
		connected |= 1u;
		if (!s_first_success) {
			s_first_success = true;
			port_log("[pad] first successful GamePad poll\n");
		}
	}
	log_error_transition(0, output[0].err);

	for (unsigned chan = 1; chan < PAD_MAX_CONTROLLERS; ++chan) {
		KPADStatus kpad;
		std::memset(&kpad, 0, sizeof(kpad));
		KPADError error = KPAD_ERROR_NO_SAMPLES;
		uint32_t count = KPADReadEx(static_cast<KPADChan>(chan), &kpad, 1, &error);
		if (count && error == KPAD_ERROR_OK) {
			output[chan].button = map_kpad_buttons(kpad.hold);
			if (kpad.extensionType == WPAD_EXT_CLASSIC) {
				output[chan].stickX = stick_value(kpad.classic.leftStick.x);
				output[chan].stickY = stick_value(-kpad.classic.leftStick.y);
				output[chan].substickX = stick_value(kpad.classic.rightStick.x);
				output[chan].substickY = stick_value(-kpad.classic.rightStick.y);
				output[chan].triggerLeft = static_cast<u8>(kpad.classic.leftTrigger * 0xff);
				output[chan].triggerRight = static_cast<u8>(kpad.classic.rightTrigger * 0xff);
			} else if (kpad.extensionType == WPAD_EXT_NUNCHUK) {
				output[chan].stickX = stick_value(kpad.nunchuk.stick.x);
				output[chan].stickY = stick_value(-kpad.nunchuk.stick.y);
			}
			output[chan].err = PAD_ERR_NONE;
			connected |= 1u << chan;
		}
		log_error_transition(chan, output[chan].err);
	}
	if (connected != s_connected_mask)
		port_log("[pad] connected-channel-mask=0x%x\n", connected);
	s_connected_mask = connected;
	return connected;
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
	port_log("[pad] rumble no-op channel=%d command=%u\n", chan, command);
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
