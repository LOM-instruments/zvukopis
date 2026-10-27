#pragma once

#include "buttons.h"

// Voice memo screen. A display task on core 1 redraws the whole screen into an
// off-screen canvas whenever what it shows changes, and pushes it to the panel
// with a partial refresh (full refresh at start and now and then to clear ghosting).

void uiStart(); // initial full refresh, then starts the display task
void uiHandleEvent(ButtonEvent event);
void uiTick(); // call regularly from loop()
void uiRequestScreenshot(); // dumps the screen as base64 on serial

// True once after PWR was pressed, or after 5 minutes without a button press
// while nothing records or plays
bool uiTakeSleepRequest();
// On: draws the sleep screen and puts the panel to sleep, returning when done.
// Off: wakes the panel and redraws, in the background.
void uiShowSleepScreen(bool on);
