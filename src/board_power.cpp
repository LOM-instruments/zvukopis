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
static const uint8_t SHTC3_ADDRESS = 0x70;   // temperature/humidity sensor
static const uint8_t QMI8658_ADDRESSES[] = {0x6B, 0x6A}; // IMU; which one depends on SA0 (0x6B on this board)

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
    // cleared. Holding PWR 1 s raises the long-press IRQ (sleep / wake); 10 s
    // restarts the whole board in hardware. USB plugged in or out also raises
    // an IRQ, so it wakes the chip too.
    pinMode(PWR_IRQ_PIN, INPUT_PULLUP);
    pmu.setIrqLevelTime(XPOWERS_AXP2101_IRQ_TIME_1S);
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
    pmu.setLongPressRestart();
    pmu.enableLongPressShutdown();
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ | XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ);

    // Per the schematic only DCDC1 (VCC3V3) and ALDO1-3 (codec, audio, e-paper)
    // are connected; switch off the rest in case the PMIC's defaults enable them
    Serial.printf("PMIC rails on: DC1 %d DC2 %d DC3 %d DC4 %d DC5 %d ALDO1-4 %d%d%d%d BLDO1-2 %d%d DLDO1-2 %d%d CPUSLDO %d\n",
                  pmu.isEnableDC1(), pmu.isEnableDC2(), pmu.isEnableDC3(), pmu.isEnableDC4(), pmu.isEnableDC5(),
                  pmu.isEnableALDO1(), pmu.isEnableALDO2(), pmu.isEnableALDO3(), pmu.isEnableALDO4(),
                  pmu.isEnableBLDO1(), pmu.isEnableBLDO2(), pmu.isEnableDLDO1(), pmu.isEnableDLDO2(),
                  pmu.isEnableCPUSLDO());
    pmu.disableDC2();
    pmu.disableDC3();
    pmu.disableDC4();
    pmu.disableALDO4();
    pmu.disableBLDO1();
    pmu.disableBLDO2();
    pmu.disableDLDO1();
    pmu.disableDLDO2();
    pmu.disableCPUSLDO();

    sensorsSleep();
    return true;
}

static bool i2cWrite(uint8_t address, const uint8_t *data, size_t len) {
    Wire.beginTransmission(address);
    Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

// The board's IMU and temperature/humidity sensor aren't used; both power up
// idle rather than asleep
void sensorsSleep() {
    const uint8_t shtc3Sleep[] = {0xB0, 0x98};
    bool sht = i2cWrite(SHTC3_ADDRESS, shtc3Sleep, sizeof(shtc3Sleep));

    int imu = -1;
    for (uint8_t address : QMI8658_ADDRESSES) {
        Wire.beginTransmission(address);
        Wire.write(0x00); // WHO_AM_I
        if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, (uint8_t)1) != 1) continue;
        if (Wire.read() != 0x05) continue;
        const uint8_t ctrl7[] = {0x08, 0x00}; // accelerometer and gyro off
        const uint8_t ctrl1[] = {0x02, 0x61}; // address auto-increment, internal oscillator off
        if (i2cWrite(address, ctrl7, sizeof(ctrl7)) && i2cWrite(address, ctrl1, sizeof(ctrl1))) imu = address;
        break;
    }
    Serial.printf("Sensors asleep: SHTC3 %s, QMI8658 %s", sht ? "yes" : "not found", imu >= 0 ? "yes" : "not found");
    if (imu >= 0) Serial.printf(" (0x%02X)", imu);
    Serial.println();
}

bool powerKeyPressed() {
    if (!pmuReady) return false;
    if (digitalRead(PWR_IRQ_PIN) == HIGH) return false; // saves an I2C read
    if (pmu.getIrqStatus() == 0) return false;
    bool pressed = pmu.isPekeyLongPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

bool powerKeyPending() { return digitalRead(PWR_IRQ_PIN) == LOW; }

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

// VBUS good, not XPowersLib's isVbusIn(), which also needs the charger's
// input limit to be inactive: a weak USB port under load (Wi-Fi, a refresh)
// then looks like no USB at all
bool usbPowered() {
    return pmuReady && pmu.isVbusGood();
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
