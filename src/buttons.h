#pragma once

#include <stdint.h>

// Side rocker (up / click / down) and BOOT, polled by a task every 10 ms, plus
// the PWR key, read from the PMIC every 100 ms.

enum ButtonEvent {
    BTN_UP, // on press
    BTN_DOWN,
    BTN_UP_RELEASE,
    BTN_DOWN_RELEASE,
    BTN_CLICK,
    BTN_LONG_CLICK, // fires once while the click button is held
    BTN_POWER,      // PWR held 2 s
    BTN_MENU,       // BOOT pressed
};

void buttonsInit();
bool buttonsNextEvent(ButtonEvent &event);
void buttonsInject(ButtonEvent event); // used by the serial debug commands
// While paused nothing is polled, so sleep code can read the PWR key itself.
// Resuming drops any events still queued.
void buttonsPause(bool paused);
// No key or PWR held and nothing queued, so light sleep won't drop a press
bool buttonsIdle();
void buttonsWakeup(bool enable); // a press wakes the chip from light sleep
// The rocker's press-in alone, for the sleep code: quick record
void buttonsClickWakeup(bool enable);
bool buttonsClickHeld();
