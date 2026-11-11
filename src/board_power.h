#pragma once

// The board's AXP2101 PMIC (I2C, SDA 41 / SCL 42) supplies the e-paper panel
// from its ALDO3 rail. Waveshare's factory firmware switches ALDO3 off after
// every refresh and the PMIC keeps that state while a battery is attached,
// so the rail must be enabled before talking to the display.

// First thing at start-up: starts the I2C bus and takes the clock chip's
// alarm off the PWR key. The chip's interrupt output is wired to the key's
// line, and an alarm left set by another firmware would hold the key down
// from the moment it goes off, for good.
void powerKeyRelease();
// Also sets the charger, switches the codec's rails on and the unused ones
// off, and puts the sensors to sleep
bool boardPowerInit();
void sensorsSleep();
void epdPowerOn();
void epdPowerOff();

int batteryPercent(); // 0-100, or -1 without a battery
bool usbPowered();
bool boardPowerReady(); // false when the PMIC did not answer at start-up: battery, USB and the PWR key are then unknown
// One reading: on battery, and under 3.30 V or at 3 % or less by the gauge
bool batteryLow();
// On battery and at the end of it. True only after two readings in a row, at
// least ten seconds apart, so that one odd reading cannot switch the board off.
bool batteryEmpty();
// The PMIC cuts the power; holding PWR or plugging USB in brings the board
// back. On battery this does not return. On USB power it does nothing: the
// board would stay off with the cable in.
void boardPowerOff();

// True once for each 1 s hold of PWR since the last call (it fires while
// still held). Holding on to 10 s restarts the board in hardware.
// Also clears the PMIC's other events (USB plugged in or out).
bool powerKeyPressed();

// Lets a PWR hold, or USB being plugged in or out, wake the chip from light
// sleep (the PMIC's IRQ line, GPIO38)
void powerKeyWakeup(bool enable);
bool powerKeyPending(); // IRQ line low: an event not yet read by powerKeyPressed()
