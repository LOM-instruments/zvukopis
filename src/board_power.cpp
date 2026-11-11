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
static const uint8_t RTC_ADDRESS = 0x51;     // PCF85063 clock chip; its time is clock.cpp's business

// The battery counts as empty at this gauge reading or under this voltage:
// with enough left to finish a recording, say so on the screen and switch off
static const int EMPTY_PERCENT = 3;
static const uint16_t EMPTY_MV = 3300;
// The gauge is believed only while the voltage does not contradict it: its
// register starts at 0 % when the PMIC has lost all power, whatever the
// charge, and an empty battery is nowhere near this voltage
static const uint16_t GAUGE_BELIEVED_BELOW_MV = 3600;
// Less than this is no reading at all: on battery the board is off before it
static const uint16_t BATTERY_PRESENT_MV = 2500;
static const uint32_t EMPTY_CONFIRM_MS = 10000;

static XPowersPMU pmu;
static bool pmuReady = false;
static int rtcControlFound = -1; // the clock chip's Control_2 as powerKeyRelease() found it, -1: no answer

// The PMIC is read from several tasks (the keys, the screen, the web page, the
// main loop), and reading a register takes two transfers: which one, then its
// value. Mixed with another task's, a read returns the other's register, and
// a battery that reads empty by such a mix-up would switch the board off.
static SemaphoreHandle_t pmuLock = nullptr;

struct PmuAccess {
    PmuAccess() {
        if (pmuLock) xSemaphoreTake(pmuLock, portMAX_DELAY);
    }
    ~PmuAccess() {
        if (pmuLock) xSemaphoreGive(pmuLock);
    }
};

bool boardPowerInit() {
    // What powerKeyRelease() found, said here: the serial port was not up then
    if (rtcControlFound < 0) {
        Serial.println("[power] the clock chip did not answer");
    } else if (rtcControlFound & 0xF8) {
        Serial.printf("[power] the clock chip had an alarm or timer set (Control_2 0x%02X), cleared: it pulls on the PWR key\n", rtcControlFound);
    }
    if (!pmuLock) pmuLock = xSemaphoreCreateMutex();
    pinMode(PWR_IRQ_PIN, INPUT_PULLUP); // also when the PMIC does not answer: the pin is read all the same
    // Not just one try: a single failed transfer, on a bus that a restart left
    // in the middle of another, say, should not cost the PMIC for the session
    for (int attempt = 0; attempt < 3 && !pmuReady; attempt++) {
        if (attempt > 0) delay(20);
        pmuReady = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN);
    }
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

    // The charger as well: 200 mA up to 4.2 V, 50 mA into a deeply empty
    // battery, done when the current is down to 25 mA. The PMIC keeps these
    // until it loses all power, so left alone they would be the last
    // firmware's, or after a power loss the chip's own, which differ.
    Serial.printf("[power] charger was at %u/%u/%u/%u (codes for precharge, current, end, voltage), now %u/%u/%u/%u: 50 mA, 200 mA, 25 mA, 4.2 V\n",
                  (unsigned)pmu.getPrechargeCurr(), (unsigned)pmu.getChargerConstantCurr(),
                  (unsigned)pmu.getChargerTerminationCurr(), (unsigned)pmu.getChargeTargetVoltage(),
                  (unsigned)XPOWERS_AXP2101_PRECHARGE_50MA, (unsigned)XPOWERS_AXP2101_CHG_CUR_200MA,
                  (unsigned)XPOWERS_AXP2101_CHG_ITERM_25MA, (unsigned)XPOWERS_AXP2101_CHG_VOL_4V2);
    pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
    pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    // Battery gauge, as in the factory firmware
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.fuelGaugeControl(true, true);

    // The PWR key belongs to the PMIC, which flags presses in its IRQ status
    // registers and pulls its IRQ line (GPIO38, 10k pull-up) low until they're
    // cleared. Holding PWR 1 s raises the long-press IRQ (sleep / wake); 10 s
    // restarts the whole board in hardware. USB plugged in or out also raises
    // an IRQ, so it wakes the chip too. From off (boardPowerOff), the same 1 s
    // hold switches the board on.
    pmu.setIrqLevelTime(XPOWERS_AXP2101_IRQ_TIME_1S);
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
    pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);
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
    // The audio rails, ALDO1 and ALDO2 (the microphone and the codec's analog
    // side), are on by the PMIC's own defaults, which is all this relied on.
    // Another firmware can have switched them off, and that holds until the
    // PMIC restarts: recordings are then silent, with no error, since the
    // codec's digital side runs from the 3.3 V rail. Their voltage stays as
    // found: the microphone's levels were measured with it.
    bool aldo1On = pmu.isEnableALDO1(), aldo2On = pmu.isEnableALDO2();
    if (!aldo1On) pmu.enableALDO1();
    if (!aldo2On) pmu.enableALDO2();
    Serial.printf("[power] audio rails: ALDO1 %u mV%s, ALDO2 %u mV%s\n",
                  pmu.getALDO1Voltage(), aldo1On ? "" : " (was off, switched on)",
                  pmu.getALDO2Voltage(), aldo2On ? "" : " (was off, switched on)");
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

// The clock chip's interrupt output pulls the PWR key's line low through a
// diode (that is how the factory firmware has an alarm switch the board on),
// and stays low until the alarm's flag is cleared. Nothing here sets an
// alarm, but one left by another firmware survives flashing for as long as
// the chip has power: when its time comes the PMIC sees PWR held for good,
// and restarts the board 10 s later (after the factory firmware, 4 s). So the
// chip's alarm, minute and timer interrupts go back to their power-on state:
// off, with their flags cleared.
void powerKeyRelease() {
    Wire.begin(PMU_SDA_PIN, PMU_SCL_PIN);
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(0x01); // Control_2: the interrupts' enable bits and flags, and in its low three bits the clock output
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(RTC_ADDRESS, (uint8_t)1) == 1) rtcControlFound = Wire.read();
    const uint8_t control2[] = {0x01, (uint8_t)(rtcControlFound < 0 ? 0x00 : rtcControlFound & 0x07)};
    const uint8_t timerMode[] = {0x11, 0x18}; // the countdown timer and its interrupt off
    i2cWrite(RTC_ADDRESS, control2, sizeof(control2));
    i2cWrite(RTC_ADDRESS, timerMode, sizeof(timerMode));
}

bool powerKeyPressed() {
    if (!pmuReady) return false;
    if (digitalRead(PWR_IRQ_PIN) == HIGH) return false; // saves an I2C read
    PmuAccess access;
    if (pmu.getIrqStatus() == 0) return false;
    bool pressed = pmu.isPekeyLongPressIrq();
    pmu.clearIrqStatus();
    return pressed;
}

// Without the PMIC the line means nothing that could be read or cleared, and
// must not keep the chip awake
bool powerKeyPending() { return pmuReady && digitalRead(PWR_IRQ_PIN) == LOW; }

void powerKeyWakeup(bool enable) {
    if (enable) {
        if (!pmuReady) return; // or a line stuck low would end every sleep at once
        gpio_wakeup_enable((gpio_num_t)PWR_IRQ_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    } else {
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        gpio_wakeup_disable((gpio_num_t)PWR_IRQ_PIN);
    }
}

int batteryPercent() {
    if (!pmuReady) return -1;
    PmuAccess access;
    if (!pmu.isBatteryConnect()) return -1;
    return pmu.getBatteryPercent();
}

// VBUS good, not XPowersLib's isVbusIn(), which also needs the charger's
// input limit to be inactive: a weak USB port under load (Wi-Fi, a refresh)
// then looks like no USB at all
bool usbPowered() {
    if (!pmuReady) return false;
    PmuAccess access;
    return pmu.isVbusGood();
}

bool boardPowerReady() { return pmuReady; }

bool batteryLow() {
    if (!pmuReady) return false;
    PmuAccess access;
    if (pmu.isVbusGood()) return false;
    uint16_t mv = pmu.getBattVoltage(); // 0 without a battery, and when it can't be read
    if (mv < BATTERY_PRESENT_MV) return false;
    if (mv < EMPTY_MV) return true;
    int percent = pmu.getBatteryPercent(); // -1 when it can't be read
    return percent >= 0 && percent <= EMPTY_PERCENT && mv < GAUGE_BELIEVED_BELOW_MV;
}

bool batteryEmpty() {
    static bool wasLow = false;
    static uint32_t lowSince = 0; // the first of the present run of low readings
    if (!batteryLow()) {
        wasLow = false;
        return false;
    }
    if (!wasLow) {
        wasLow = true;
        lowSince = millis();
    }
    return millis() - lowSince >= EMPTY_CONFIRM_MS;
}

void boardPowerOff() {
    if (!pmuReady) return;
    PmuAccess access;
    if (pmu.isVbusGood()) return; // the PMIC would switch off all the same
    Serial.println("[power] switching off");
    Serial.flush();
    // The IRQ line held low by an event still waiting may count as a reason
    // to switch on again (the PMIC can be made that way)
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    // The power goes within milliseconds of the write; more than one try, in
    // case a transfer fails
    for (int attempt = 0; attempt < 3; attempt++) {
        pmu.shutdown();
        delay(200);
    }
}

void epdPowerOn() {
    if (!pmuReady) return;
    {
        PmuAccess access;
        pmu.setALDO3Voltage(3300);
        pmu.enableALDO3();
    }
    delay(10); // let the rail settle before resetting the panel
}

void epdPowerOff() {
    if (!pmuReady) return;
    PmuAccess access;
    pmu.disableALDO3();
}
