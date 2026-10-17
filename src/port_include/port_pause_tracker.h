#ifndef PORT_PAUSE_TRACKER_H
#define PORT_PAUSE_TRACKER_H

// The pause menu's progress tracker (src/pause_tracker.cpp): an area's Shine
// Sprites and blue coins, got out of all there are, at the menu's bottom
// left. L and R step through the areas visited. The pause menu drives it
// (patch zzzz-tracker-01).

class TMarioGamePad;
namespace JDrama {
struct TRect;
}

// SMS_PAUSE_TRACKER (pause_tracker = off in settings.txt, platform): 0 turns
// the tracker off. Eclipse builds turn it off: BSE draws its own.
extern "C" int port_pause_tracker;

// As the menu appears: back to the area Mario is in, the counts read again.
void PauseTrackerReset();
// Once a frame while the menu is open and has the pad: L and R.
void PauseTrackerMove(TMarioGamePad* pad);
// With the menu, fading with it; viewport is the menu's 2D area.
void PauseTrackerDraw(const JDrama::TRect& viewport, u8 alpha);

#endif
