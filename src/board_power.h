#pragma once

// The board's AXP2101 PMIC (I2C, SDA 41 / SCL 42) supplies the e-paper panel
// from its ALDO3 rail. Waveshare's factory firmware switches ALDO3 off after
// every refresh and the PMIC keeps that state while a battery is attached,
// so the rail must be enabled before talking to the display.

bool boardPowerInit();
void epdPowerOn();
void epdPowerOff();

int batteryPercent(); // 0-100, or -1 without a battery
bool usbPowered();
