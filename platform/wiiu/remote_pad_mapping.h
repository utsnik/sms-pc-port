#ifndef SMS_WIIU_REMOTE_PAD_MAPPING_H
#define SMS_WIIU_REMOTE_PAD_MAPPING_H

#include <stdint.h>

namespace sms_wiiu_remote {

enum Button {
	kButtonA = 1u << 0,
	kButtonB = 1u << 1,
	kButton1 = 1u << 2,
	kButton2 = 1u << 3,
	kButtonPlus = 1u << 4,
	kButtonMinus = 1u << 5,
};

struct Input {
	bool up;
	bool right;
	bool down;
	bool left;
	uint32_t buttons;
};

enum GameButton {
	kGameA = 1u << 0,
	kGameB = 1u << 1,
	kGameX = 1u << 2,
	kGameStart = 1u << 3,
	kGameL = 1u << 4,
	kGameR = 1u << 5,
};

struct Output {
	uint32_t buttons;
	int8_t stick_x;
	int8_t stick_y;
	uint8_t trigger_l;
	uint8_t trigger_r;
};

/* Sideways, extension-free Wii Remote mapping. D-pad rotates into the main
 * stick; held diagonal directions produce full diagonal values. */
inline Output mapSideways(const Input& input)
{
	Output out = {};
	out.stick_x = (input.down ? 100 : 0) - (input.up ? 100 : 0);
	out.stick_y = (input.right ? 100 : 0) - (input.left ? 100 : 0);
	const uint32_t b = input.buttons;
	if (b & kButton2) out.buttons |= kGameA;
	if (b & kButton1) out.buttons |= kGameB;
	if (b & kButtonA) out.buttons |= kGameX;
	if (b & kButtonPlus) out.buttons |= kGameStart;
	if (b & kButtonMinus) {
		out.buttons |= kGameL;
		out.trigger_l = 0xff;
	}
	if (b & kButtonB) {
		out.buttons |= kGameR;
		out.trigger_r = 0xff;
	}
	return out;
}

}  // namespace sms_wiiu_remote

#endif
