#pragma once

#include <stdint.h>

// Side rocker (up / click / down) and the BOOT button, polled by a task every 10 ms,
// plus the PWR key, read from the PMIC every 100 ms.
// Holding BOOT records directly (start/stop timing doesn't wait for the UI);
// a short BOOT tap acts like a click.

enum ButtonEvent {
    BTN_UP,
    BTN_DOWN,
    BTN_CLICK,
    BTN_LONG_CLICK, // fires once while the click button is held
    BTN_POWER,      // short press of PWR
};

void buttonsInit();
bool buttonsNextEvent(ButtonEvent &event);
void buttonsInject(ButtonEvent event); // used by the serial debug commands
// While paused nothing is polled, so sleep code can read the PWR key itself.
// Resuming drops any events still queued.
void buttonsPause(bool paused);
