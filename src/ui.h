#pragma once

#include "buttons.h"

// Voice moment screen. A display task on core 1 redraws the whole screen into an
// off-screen canvas whenever what it shows changes, and pushes it to the panel
// with a partial refresh (full refresh at start and now and then to clear ghosting).

void uiStart(); // initial full refresh, then starts the display task
void uiHandleEvent(ButtonEvent event);
void uiTick(); // call regularly from loop()
void uiRequestScreenshot(); // dumps the screen as base64 on serial

// True once after PWR was pressed, or after 5 minutes without a button press
// while nothing records or plays
bool uiTakeSleepRequest();
// For light sleep between button presses: true when the display task, asked
// just now, has looked at the screen and found nothing left to draw. It waits
// a few milliseconds for that look, so call it last, right before the nap.
// `since` (the last wake-up, millis) is no longer needed and is ignored.
bool uiReadyToNap(uint32_t since);
// Until the screen next changes on its own: the clock's minute, a result or
// notice going away, the idle full refresh, auto-sleep
uint32_t uiMsUntilNextChange();
// On: draws the sleep screen and puts the panel to sleep, returning when done.
// Off: wakes the panel and redraws, in the background (a partial refresh; the
// full one follows once the buttons have been left alone).
void uiShowSleepScreen(bool on);
bool uiCanRecord(); // not while the SD card is open as a USB drive
// Woken by a hold of the rocker: shows the recording screen (and waits for the
// panel, so the refresh is over before the recording starts), then the
// recording itself takes over the screen
void uiQuickRecordScreen();
void uiQuickRecordStarted();
// The battery is empty: a page saying so (one full refresh), then the panel
// sleeps. Returns when that is done; the caller then powers the board off.
// Should the board stay on after all, the page gives way to the screen at the
// next key press, or at the next sleep or wake-up.
void uiShowBatteryEmpty();
