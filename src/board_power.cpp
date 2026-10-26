#include "board_power.h"

#include <Arduino.h>
#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

static const int PMU_SDA_PIN = 41;
static const int PMU_SCL_PIN = 42;

static XPowersPMU pmu;
static bool pmuReady = false;

bool boardPowerInit() {
    pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN);
    if (!pmuReady) {
        Serial.println("AXP2101 PMU not found");
        return false;
    }
    Serial.printf("AXP2101 found, e-paper rail (ALDO3) was %s, VBUS %s, VBAT %u mV\n",
                  pmu.isEnableALDO3() ? "ON" : "OFF",
                  pmu.isVbusIn() ? "present" : "absent",
                  pmu.getBattVoltage());

    // Same limits as Waveshare's factory firmware. With the PMIC defaults the board
    // browns out and powers off when the panel draws its refresh current.
    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    pmu.setSysPowerDownVoltage(2600);

    // Battery gauge, as in the factory firmware
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.fuelGaugeControl(true, true);

    // The PWR key isn't wired to the ESP32; the PMIC flags presses in its IRQ
    // status registers, which powerKeyPressed() polls
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    return true;
}

bool powerKeyPressed() {
    if (!pmuReady) return false;
    if (pmu.getIrqStatus() == 0) return false;
    bool pressed = pmu.isPekeyShortPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

int batteryPercent() {
    if (!pmuReady || !pmu.isBatteryConnect()) return -1;
    return pmu.getBatteryPercent();
}

bool usbPowered() {
    return pmuReady && pmu.isVbusIn();
}

void epdPowerOn() {
    if (!pmuReady) return;
    pmu.setALDO3Voltage(3300);
    pmu.enableALDO3();
    delay(10); // let the rail settle before resetting the panel
}

void epdPowerOff() {
    if (!pmuReady) return;
    pmu.disableALDO3();
}
