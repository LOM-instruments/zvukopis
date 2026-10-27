#pragma once

// The board's AXP2101 PMIC (I2C, SDA 41 / SCL 42) supplies the e-paper panel
// from its ALDO3 rail. Waveshare's factory firmware switches ALDO3 off after
// every refresh and the PMIC keeps that state while a battery is attached,
// so the rail must be enabled before talking to the display.

bool boardPowerInit(); // also switches off unused rails and puts the sensors to sleep
void sensorsSleep();
void epdPowerOn();
void epdPowerOff();

int batteryPercent(); // 0-100, or -1 without a battery
bool usbPowered();

// True once for each short press of PWR since the last call. A long hold
// (about 6 s) still powers the board off in hardware.
bool powerKeyPressed();

// Lets a PWR press wake the chip from light sleep (the PMIC's IRQ line, GPIO38)
void powerKeyWakeup(bool enable);
bool powerKeyPending(); // IRQ line low: a press not yet read by powerKeyPressed()
