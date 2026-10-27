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

// True once for each 2 s hold of PWR since the last call (it fires while
// still held). Holding on to 10 s restarts the board in hardware.
// Also clears the PMIC's other events (USB plugged in or out).
bool powerKeyPressed();

// Lets a PWR hold, or USB being plugged in or out, wake the chip from light
// sleep (the PMIC's IRQ line, GPIO38)
void powerKeyWakeup(bool enable);
bool powerKeyPending(); // IRQ line low: an event not yet read by powerKeyPressed()
