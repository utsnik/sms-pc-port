#include "../remote_pad_mapping.h"

#include <cstdio>

namespace {

int failures;

void check(bool ok, const char* name)
{
	std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) ++failures;
}

sms_wiiu_remote::Input directions(bool up, bool right, bool down, bool left)
{
	sms_wiiu_remote::Input input = {up, right, down, left, 0};
	return input;
}

}  // namespace

int main()
{
	using namespace sms_wiiu_remote;
	Output out = mapSideways(directions(true, false, false, false));
	check(out.stick_x == -100 && out.stick_y == 0 && out.buttons == 0,
	      "physical Up maps to stick left");
	out = mapSideways(directions(false, true, false, false));
	check(out.stick_x == 0 && out.stick_y == 100 && out.buttons == 0,
	      "physical Right maps to stick up");
	out = mapSideways(directions(false, false, true, false));
	check(out.stick_x == 100 && out.stick_y == 0 && out.buttons == 0,
	      "physical Down maps to stick right");
	out = mapSideways(directions(false, false, false, true));
	check(out.stick_x == 0 && out.stick_y == -100 && out.buttons == 0,
	      "physical Left maps to stick down");
	out = mapSideways(directions(true, true, false, false));
	check(out.stick_x == -100 && out.stick_y == 100, "diagonal keeps both rotated axes");
	out = mapSideways(directions(false, true, true, false));
	check(out.stick_x == 100 && out.stick_y == 100, "second diagonal combines both axes");
	out = mapSideways(directions(false, false, true, true));
	check(out.stick_x == 100 && out.stick_y == -100, "third diagonal combines both axes");
	out = mapSideways(directions(true, false, false, true));
	check(out.stick_x == -100 && out.stick_y == -100, "fourth diagonal combines both axes");
	out = mapSideways(directions(false, false, false, false));
	check(out.stick_x == 0 && out.stick_y == 0 && out.buttons == 0 &&
	          out.trigger_l == 0 && out.trigger_r == 0,
	      "neutral input has no buttons, axes, or triggers");

	Input buttons = directions(false, false, false, false);
	buttons.buttons = kButtonA | kButtonB | kButton1 | kButton2 | kButtonPlus | kButtonMinus;
	out = mapSideways(buttons);
	check((out.buttons & (kGameA | kGameB | kGameX | kGameStart | kGameL | kGameR)) ==
	          (kGameA | kGameB | kGameX | kGameStart | kGameL | kGameR) &&
	          out.trigger_l == 0xff && out.trigger_r == 0xff,
	      "2/1/A/B/Minus/Plus map to A/B/X/R/L/Start");
	buttons.buttons = kButtonB;
	out = mapSideways(buttons);
	check((out.buttons & kGameR) == kGameR && out.trigger_r == 0xff &&
	          (out.buttons & (kGameA | kGameB | kGameX | kGameStart | kGameL)) == 0,
	      "B is only the full R trigger");
	return failures ? 1 : 0;
}
