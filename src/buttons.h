#pragma once

#include <stdint.h>

// Side rocker (up / click / down) and the BOOT button, polled by a task every 10 ms.
// Holding BOOT records directly (start/stop timing doesn't wait for the UI);
// a short BOOT tap acts like a click.

enum ButtonEvent {
    BTN_UP,
    BTN_DOWN,
    BTN_CLICK,
    BTN_LONG_CLICK, // fires once while the click button is held
};

void buttonsInit();
bool buttonsNextEvent(ButtonEvent &event);
void buttonsInject(ButtonEvent event); // used by the serial debug commands
