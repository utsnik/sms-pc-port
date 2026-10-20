#include "runtime.h"

#include <dolphin/pad.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>

/* Set when the port itself asks for the Menu; vi.cpp drains the GPU before the
 * ProcUI pump that releases the foreground. */
extern "C" volatile int sms_wiiu_menu_exit_pending = 0;

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

/* Unattended test input is compiled in but stays off unless a file named
 * "autopress" exists in the app directory on the SD card (Cemu test SD only).
 * -DSMS_WIIU_AUTOPRESS=0 removes it entirely. */
#ifndef SMS_WIIU_AUTOPRESS
#define SMS_WIIU_AUTOPRESS 1
#endif

#if SMS_WIIU_AUTOPRESS
bool autopress_marker_present()
{
	char path[160];
	std::snprintf(path, sizeof(path), "%sautopress", sms_wiiu_app_root());
	FILE* file = std::fopen(path, "rb");
	if (!file)
		return false;
	std::fclose(file);
	return true;
}

/* A script in the "autoplay" file drives the pad by retrace field, in the
 * desktop SMS_AUTOPRESS form (control@field[+hold], comma or newline
 * separated). Lines NAME=VALUE are set as environment variables instead
 * (SMS_WARP, SMS_SKIP_MOVIES), so getenv-driven debug switches work here. */
struct ScriptPress {
	u16 button;
	s8 sx, sy;
	uint32_t at, until;
};
ScriptPress s_script[48];
int s_script_count;
volatile uint32_t s_field;

bool script_control(const char* name, ScriptPress& p)
{
	static const struct { const char* name; u16 button; s8 sx, sy; } kControls[] = {
		{ "a", PAD_BUTTON_A, 0, 0 }, { "b", PAD_BUTTON_B, 0, 0 }, { "x", PAD_BUTTON_X, 0, 0 },
		{ "y", PAD_BUTTON_Y, 0, 0 }, { "start", PAD_BUTTON_START, 0, 0 }, { "z", PAD_TRIGGER_Z, 0, 0 },
		{ "l", PAD_TRIGGER_L, 0, 0 }, { "r", PAD_TRIGGER_R, 0, 0 }, { "up", PAD_BUTTON_UP, 0, 0 },
		{ "down", PAD_BUTTON_DOWN, 0, 0 }, { "left", PAD_BUTTON_LEFT, 0, 0 },
		{ "right", PAD_BUTTON_RIGHT, 0, 0 }, { "stick_left", 0, -100, 0 }, { "stick_right", 0, 100, 0 },
		{ "stick_up", 0, 0, 100 }, { "stick_down", 0, 0, -100 },
	};
	for (const auto& c : kControls)
		if (strcasecmp(c.name, name) == 0) {
			p.button = c.button;
			p.sx = c.sx;
			p.sy = c.sy;
			return true;
		}
	return false;
}

void script_load(FILE* f)
{
	char buf[2048];
	size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	char* save = nullptr;
	for (char* tok = strtok_r(buf, ",\r\n", &save); tok; tok = strtok_r(nullptr, ",\r\n", &save)) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		if (char* eq = std::strchr(tok, '=')) {
			*eq = 0;
			setenv(tok, eq + 1, 1);
			port_log("[pad] autoplay env %s=%s\n", tok, eq + 1);
			continue;
		}
		char* at = std::strchr(tok, '@');
		if (!at || s_script_count >= (int)(sizeof(s_script) / sizeof(s_script[0])))
			continue;
		*at = 0;
		ScriptPress p{};
		if (!script_control(tok, p)) {
			port_log("[pad] autoplay: unknown control '%s'\n", tok);
			continue;
		}
		uint32_t hold = 8;
		if (const char* plus = std::strchr(at + 1, '+'))
			hold = (uint32_t)atoi(plus + 1);
		p.at = (uint32_t)atoi(at + 1);
		p.until = p.at + hold;
		s_script[s_script_count++] = p;
	}
	port_log("[pad] autoplay: %d scripted presses\n", s_script_count);
}
#endif

/* Sunshine reads R/L as analog (a half press squirts FLUDD while walking, a
 * full press stops and aims). The Wii U pads have digital triggers, so the
 * shoulder (L/R) is a full press and the trigger (ZL/ZR) a half press: analog
 * ~0x80 with no digital bit. Stick clicks stand in for GameCube Z. */
const u8 kHalfPress = 0x80;

void set_triggers(PADStatus& out, bool full_l, bool half_l, bool full_r, bool half_r)
{
	if (full_l) {
		out.button |= PAD_TRIGGER_L;
		out.triggerLeft = 0xff;
	} else if (half_l) {
		out.triggerLeft = kHalfPress;
	}
	if (full_r) {
		out.button |= PAD_TRIGGER_R;
		out.triggerRight = 0xff;
	} else if (half_r) {
		out.triggerRight = kHalfPress;
	}
}

u16 map_face_dpad(uint32_t buttons, uint32_t left, uint32_t right, uint32_t down, uint32_t up,
                  uint32_t a, uint32_t b, uint32_t x, uint32_t y, uint32_t plus)
{
	u16 result = 0;
	if (buttons & left) result |= PAD_BUTTON_LEFT;
	if (buttons & right) result |= PAD_BUTTON_RIGHT;
	if (buttons & down) result |= PAD_BUTTON_DOWN;
	if (buttons & up) result |= PAD_BUTTON_UP;
	if (buttons & a) result |= PAD_BUTTON_A;
	if (buttons & b) result |= PAD_BUTTON_B;
	if (buttons & x) result |= PAD_BUTTON_X;
	if (buttons & y) result |= PAD_BUTTON_Y;
	if (buttons & plus) result |= PAD_BUTTON_START;
	return result;
}

void map_vpad(uint32_t b, PADStatus& out)
{
	out.button = map_face_dpad(b, VPAD_BUTTON_LEFT, VPAD_BUTTON_RIGHT, VPAD_BUTTON_DOWN, VPAD_BUTTON_UP,
	                           VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_X, VPAD_BUTTON_Y, VPAD_BUTTON_PLUS);
	if (b & (VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R))
		out.button |= PAD_TRIGGER_Z;
	set_triggers(out, b & VPAD_BUTTON_L, b & VPAD_BUTTON_ZL, b & VPAD_BUTTON_R, b & VPAD_BUTTON_ZR);
}

/* Classic and Pro Controller buttons arrive in the extension's own hold word. */
void map_pro(uint32_t b, PADStatus& out)
{
	out.button = map_face_dpad(b, WPAD_PRO_BUTTON_LEFT, WPAD_PRO_BUTTON_RIGHT, WPAD_PRO_BUTTON_DOWN,
	                           WPAD_PRO_BUTTON_UP, WPAD_PRO_BUTTON_A, WPAD_PRO_BUTTON_B, WPAD_PRO_BUTTON_X,
	                           WPAD_PRO_BUTTON_Y, WPAD_PRO_BUTTON_PLUS);
	if (b & (WPAD_PRO_BUTTON_STICK_L | WPAD_PRO_BUTTON_STICK_R))
		out.button |= PAD_TRIGGER_Z;
	set_triggers(out, b & WPAD_PRO_BUTTON_L, b & WPAD_PRO_BUTTON_ZL, b & WPAD_PRO_BUTTON_R,
	             b & WPAD_PRO_BUTTON_ZR);
}

/* The Classic Controller has analog L/R (taken as is) and digital ZL/ZR as Z. */
u16 map_classic_buttons(uint32_t b)
{
	u16 result = map_face_dpad(b, WPAD_CLASSIC_BUTTON_LEFT, WPAD_CLASSIC_BUTTON_RIGHT, WPAD_CLASSIC_BUTTON_DOWN,
	                           WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B,
	                           WPAD_CLASSIC_BUTTON_X, WPAD_CLASSIC_BUTTON_Y, WPAD_CLASSIC_BUTTON_PLUS);
	if (b & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR)) result |= PAD_TRIGGER_Z;
	if (b & WPAD_CLASSIC_BUTTON_L) result |= PAD_TRIGGER_L;
	if (b & WPAD_CLASSIC_BUTTON_R) result |= PAD_TRIGGER_R;
	return result;
}

/* Wii Remote, with or without a Nunchuk. With a Nunchuk there is no R/L, so
 * B squirts (R), C is L and Minus takes over GameCube B; the D-pad also drives
 * the C-stick (camera) while still acting as a D-pad for menus. */
void map_remote(const KPADStatus& kpad, PADStatus& out)
{
	uint32_t b = kpad.hold;
	const bool nunchuk = kpad.extensionType == WPAD_EXT_NUNCHUK;
	if (nunchuk)
		b |= kpad.nunchuk.hold;
	out.button = map_face_dpad(b, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_UP,
	                           WPAD_BUTTON_A, nunchuk ? WPAD_BUTTON_MINUS : WPAD_BUTTON_B, WPAD_BUTTON_1,
	                           WPAD_BUTTON_2, WPAD_BUTTON_PLUS);
	if (!nunchuk)
		return;
	if (b & (WPAD_BUTTON_Z | WPAD_NUNCHUK_BUTTON_Z)) out.button |= PAD_TRIGGER_Z;
	set_triggers(out, b & WPAD_BUTTON_C, false, b & WPAD_BUTTON_B, false);
	out.substickX = (b & WPAD_BUTTON_RIGHT ? 100 : 0) - (b & WPAD_BUTTON_LEFT ? 100 : 0);
	out.substickY = (b & WPAD_BUTTON_UP ? 100 : 0) - (b & WPAD_BUTTON_DOWN ? 100 : 0);
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
		map_pro(kpad.pro.hold, out);
		out.stickX = stick_value(kpad.pro.leftStick.x);
		out.stickY = stick_value(kpad.pro.leftStick.y);
		out.substickX = stick_value(kpad.pro.rightStick.x);
		out.substickY = stick_value(kpad.pro.rightStick.y);
	} else if (kpad.extensionType == WPAD_EXT_CLASSIC) {
		out.button = map_classic_buttons(kpad.classic.hold);
		out.stickX = stick_value(kpad.classic.leftStick.x);
		out.stickY = stick_value(kpad.classic.leftStick.y);
		out.substickX = stick_value(kpad.classic.rightStick.x);
		out.substickY = stick_value(kpad.classic.rightStick.y);
		out.triggerLeft = static_cast<u8>(kpad.classic.leftTrigger * 0xff);
		out.triggerRight = static_cast<u8>(kpad.classic.rightTrigger * 0xff);
	} else {
		map_remote(kpad, out);
		if (kpad.extensionType == WPAD_EXT_NUNCHUK) {
			out.stickX = stick_value(kpad.nunchuk.stick.x);
			out.stickY = stick_value(kpad.nunchuk.stick.y);
		}
	}
	out.err = PAD_ERR_NONE;
	return true;
}

bool s_rumble[PAD_MAX_CONTROLLERS];
unsigned s_rumble_age[PAD_MAX_CONTROLLERS];

void apply_motor(unsigned chan, bool on)
{
	if (chan == 0) {
		if (on) {
			uint8_t pattern[120];
			std::memset(pattern, 0xff, sizeof(pattern));
			VPADControlMotor(VPAD_CHAN_0, pattern, sizeof(pattern));
		} else {
			VPADStopMotor(VPAD_CHAN_0);
		}
		WPADControlMotor(WPAD_CHAN_0, on ? TRUE : FALSE);
	} else {
		WPADControlMotor(static_cast<WPADChan>(chan), on ? TRUE : FALSE);
	}
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
		map_vpad(vpad.hold, out);
		out.stickX = stick_value(vpad.leftStick.x);
		out.stickY = stick_value(vpad.leftStick.y);
		out.substickX = stick_value(vpad.rightStick.x);
		out.substickY = stick_value(vpad.rightStick.y);
		if (have_kpad0) {
			out.button |= kpad0.button;
			if (kpad0.triggerLeft > out.triggerLeft) out.triggerLeft = kpad0.triggerLeft;
			if (kpad0.triggerRight > out.triggerRight) out.triggerRight = kpad0.triggerRight;
			if (!out.substickX && !out.substickY) {
				out.substickX = kpad0.substickX;
				out.substickY = kpad0.substickY;
			}
		}
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
#if SMS_WIIU_AUTOPRESS
	/* Unattended test runs: press START+A on port 0 for 6 frames at a few
	 * points after boot, so the title screen moves on to a stage with nobody
	 * at the console. Build with -DSMS_WIIU_AUTOPRESS=0 for a release. */
	{
		static uint32_t s_poll_count;
		static int s_marker = -1;
		static const uint32_t kPressAt[] = { 1200, 1500, 1800, 2400, 3000 };
		if (s_marker < 0) {
			s_marker = autopress_marker_present() ? 1 : 0;
			port_log("[pad] autopress marker %s\n", s_marker ? "present: unattended input ON" : "absent: off");
		}
		uint32_t n = s_marker ? ++s_poll_count : 0;
		/* "autoplay" marker: keep going past file select into a stage: A
		 * every 120 polls (menus, dialogue), START+A every 600 (cutscene
		 * skip; a pause it opens is closed by the next A). Runs 300 s. */
		static int s_autoplay = -1;
		if (s_autoplay < 0 && s_marker) {
			char path[192];
			std::snprintf(path, sizeof(path), "%sautoplay", sms_wiiu_app_root());
			FILE* f = std::fopen(path, "rb");
			s_autoplay = f ? 1 : 0;
			if (f) {
				script_load(f);
				std::fclose(f);
			}
			port_log("[pad] autoplay marker %s\n", s_autoplay ? "present" : "absent");
		}
		/* Keep the virtual port-0 pad connected between presses: with the
		 * GamePad asleep it reads as unplugged and the menus stall. */
		if (s_autoplay > 0 && n >= 1200 && s_status[0].err != PAD_ERR_NONE)
			std::memset(&s_status[0], 0, sizeof(PADStatus));
		if (s_script_count) {
			const uint32_t field = s_field;
			for (int i = 0; i < s_script_count; i++) {
				const ScriptPress& p = s_script[i];
				if (field < p.at || field >= p.until)
					continue;
				if (field == p.at)
					port_log("[pad] autoplay press %d at field %u\n", i, (unsigned)field);
				s_status[0].button |= p.button;
				if (p.sx) s_status[0].stickX = p.sx;
				if (p.sy) s_status[0].stickY = p.sy;
			}
		} else if (s_autoplay > 0 && n >= 3600 && (n % 120) < 6) {
			s_status[0].button |= PAD_BUTTON_A;
			if ((n % 600) < 6) s_status[0].button |= PAD_BUTTON_START;
		}
		for (uint32_t at : kPressAt) {
			if (!s_script_count && n >= at && n < at + 6) {
				if (s_status[0].err != PAD_ERR_NONE)
					std::memset(&s_status[0], 0, sizeof(PADStatus));
				s_status[0].button |= PAD_BUTTON_START | PAD_BUTTON_A;
				if (n == at)
					port_log("[pad] autopress START+A at poll %u\n", (unsigned)n);
			}
		}
		/* Unattended runs end themselves, so the next wiiload finds the
		 * console at the Menu (wiiload into a running title wedges it). */
		static OSTime s_marker_start;
		static bool s_exit_requested;
		if (s_marker && !s_exit_requested) {
			const OSTime now = OSGetTime();
			if (!s_marker_start)
				s_marker_start = now;
			else if (OSTicksToSeconds(now - s_marker_start) >= (s_autoplay > 0 ? 300 : 180)) {
				s_exit_requested = true;
				sms_wiiu_menu_exit_pending = 1;
				port_log("[pad] autopress run over (180 s): returning to the Menu\n");
				SYSLaunchMenu();
			}
		}
	}
#endif
	if (s_rumble[0] && ++s_rumble_age[0] >= 600) { // re-arm the GamePad pattern
		s_rumble_age[0] = 0;
		apply_motor(0, true);
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
/* Rumble: command PAD_MOTOR_RUMBLE keeps the motor on, STOP/STOP_HARD end it.
 * Port 0 drives the GamePad and Wii Remote 1, ports 1-3 Wii Remotes 2-4. The
 * GamePad pattern lasts a limited time, so it is re-armed from the poll. */
void PADControlMotor(s32 chan, u32 command)
{
	if (chan < 0 || chan >= PAD_MAX_CONTROLLERS || !s_initialized)
		return;
	bool on = command == PAD_MOTOR_RUMBLE;
	if (s_rumble[chan] == on)
		return;
	s_rumble[chan] = on;
	s_rumble_age[chan] = 0;
	apply_motor(chan, on);
}
void PADControlAllMotors(const u32* commands)
{
	if (!commands)
		return;
	for (s32 chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan)
		PADControlMotor(chan, commands[chan]);
}
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

extern "C" void port_pad_autopress_field(uint32_t field)
{
#if SMS_WIIU_AUTOPRESS
	s_field = field;
#else
	(void)field;
#endif
}
