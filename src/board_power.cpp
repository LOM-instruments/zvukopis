#include "board_power.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

static const int PMU_SDA_PIN = 41;
static const int PMU_SCL_PIN = 42;
static const int PWR_IRQ_PIN = 38;

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

    // The PWR key belongs to the PMIC, which flags presses in its IRQ status
    // registers and pulls its IRQ line (GPIO38, 10k pull-up) low until they're
    // cleared. Only the short press is enabled, so a low line means PWR.
    pinMode(PWR_IRQ_PIN, INPUT_PULLUP);
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    return true;
}

bool powerKeyPressed() {
    if (!pmuReady) return false;
    if (digitalRead(PWR_IRQ_PIN) == HIGH) return false; // saves an I2C read
    if (pmu.getIrqStatus() == 0) return false;
    bool pressed = pmu.isPekeyShortPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

void powerKeyWakeup(bool enable) {
    if (enable) {
        gpio_wakeup_enable((gpio_num_t)PWR_IRQ_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        gpio_wakeup_disable((gpio_num_t)PWR_IRQ_PIN);
    }
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
