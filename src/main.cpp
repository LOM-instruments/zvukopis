#include <Arduino.h>
#include <esp_sleep.h>
#include <mbedtls/base64.h>
#include <soc/usb_serial_jtag_reg.h>

#include "EPD_3in97.h"

#include "audio.h"
#include "board_power.h"
#include "buttons.h"
#include "clock.h"
#include "ui.h"

// How often the chip checks the PWR key while "asleep" on USB power
static const uint32_t USB_POLL_MS = 200;
// Plenty for this app (16 kHz audio, a 1 bpp screen); 240 MHz draws more for nothing
static const uint32_t CPU_MHZ = 80;
static const uint32_t MIN_NAP_MS = 20; // shorter naps aren't worth the wake-up

static uint32_t napTotalMs = 0, napCount = 0, wokeAt = 0;
// These survive resets (not power-offs), so a session on battery can be
// checked after plugging in, which resets the board when the port is opened
RTC_NOINIT_ATTR static uint32_t bootCount;
RTC_NOINIT_ATTR static uint32_t lastUpMs, lastNapMs, lastNaps, lastSleepMs;
static uint32_t sleepTotalMs = 0; // in sleep mode, on battery

// Light sleep stops the USB clock and the host gives up on the port. Pulling
// D+ low for a moment looks like an unplug and replug, so the host enumerates
// it again.
static void usbReconnect() {
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
    delay(50);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
}

static bool onUsbPower() {
    static bool usb = false;
    static uint32_t checkedAt = 0;
    if (checkedAt == 0 || millis() - checkedAt > 1000) {
        usb = usbPowered();
        checkedAt = millis();
    }
    return usb;
}

// Light sleep for up to `ms`, until a key, PWR or the timer wakes the chip
static void nap(uint32_t ms) {
    buttonsWakeup(true);
    powerKeyWakeup(true);
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    uint32_t start = millis();
    esp_light_sleep_start();
    wokeAt = millis();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    buttonsWakeup(false);
    powerKeyWakeup(false);
    napTotalMs += wokeAt - start;
    napCount++;
    lastUpMs = wokeAt;
    lastNapMs = napTotalMs;
    lastNaps = napCount;
}

// Between button presses the chip light-sleeps (about 1 mA instead of tens):
// the e-paper keeps showing the screen, PSRAM keeps the moments, and a key press
// wakes it within a few ms. A timer wakes it for the clock's next minute.
// Not on USB power, where it would cut off the serial port.
static void napIfIdle() {
    if (!audioAsleep() || clockSyncing() || !buttonsIdle() || !uiReadyToNap(wokeAt)) return;
    if (onUsbPower()) return;
    uint32_t ms = uiMsUntilNextChange();
    if (ms < MIN_NAP_MS) return;
    nap(ms);
    if (usbPowered()) usbReconnect(); // plugged in while napping (that wakes it too)
}

static void printPowerStats() {
    uint32_t up = millis();
    Serial.printf("[power] up %lu s, sleep mode %lu s, light-slept %lu s in %lu naps, CPU %lu MHz, %s, battery %d%%\n",
                  up / 1000, sleepTotalMs / 1000, napTotalMs / 1000, napCount,
                  getCpuFrequencyMhz(), usbPowered() ? "on USB" : "on battery", batteryPercent());
}

// Light sleep keeps PSRAM powered, so moments held in memory survive it.
// A PWR hold pulls the PMIC's IRQ line low, which wakes the chip; so does USB
// being plugged in, and a timer once a minute in case that was missed.
// On USB power the chip stays awake instead, so its serial port keeps working;
// power isn't scarce then. A 'p' over serial also wakes it in that case.
// Returns false when the power source changed, to be called again.
static const uint64_t SLEEP_RECHECK_US = 60 * 1000 * 1000ULL;

static bool sleepUntilPowerKey() {
    if (usbPowered()) {
        while (Serial.available()) Serial.read();
        for (;;) {
            delay(USB_POLL_MS);
            if (powerKeyPressed()) return true;
            while (Serial.available()) {
                int c = Serial.read();
                if (c == 'p') return true;
                if (c == 's') uiRequestScreenshot(); // of the sleep screen
            }
            if (!usbPowered()) return false; // unplugged: continue in light sleep
        }
    }
    powerKeyWakeup(true);
    esp_sleep_enable_timer_wakeup(SLEEP_RECHECK_US);
    uint32_t start = millis();
    bool pressed;
    do {
        esp_light_sleep_start();
        pressed = powerKeyPressed();
    } while (!pressed && !usbPowered());
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    powerKeyWakeup(false);
    sleepTotalMs += millis() - start;
    lastUpMs = millis();
    lastSleepMs = sleepTotalMs;
    if (usbPowered()) usbReconnect(); // plugged in while asleep
    return pressed;
}

static void goToSleep() {
    Serial.println("[sleep] going to sleep");
    audioSuspend(); // saves a recording in progress first
    clockPauseSync(true);
    buttonsPause(true);
    uiShowSleepScreen(true);
    Serial.println("[sleep] asleep");
    Serial.flush();

    powerKeyPressed(); // drop a press made while the sleep screen was drawn
    while (!sleepUntilPowerKey()) {
        Serial.printf("[sleep] now on %s\n", usbPowered() ? "USB" : "battery");
    }

    Serial.println("[sleep] woke up");
    uiShowSleepScreen(false);
    buttonsPause(false);
    clockPauseSync(false);
}

// Debug: newest moment's raw samples as base64, for analysis on a computer
static void dumpNewestMoment() {
    Moment moment;
    if (!momentGet(0, moment) || !moment.samples) {
        Serial.println("No moment in memory to dump");
        return;
    }
    Serial.printf("AUDIO-BEGIN moment=%d samples=%u\n", moment.number, moment.length);
    const uint8_t *bytes = (const uint8_t *)moment.samples;
    size_t total = moment.length * sizeof(int16_t);
    unsigned char line[80];
    for (size_t i = 0; i < total; i += 57) {
        size_t outLen = 0;
        mbedtls_base64_encode(line, sizeof(line), &outLen, bytes + i, min((size_t)57, total - i));
        Serial.write(line, outLen);
        Serial.println();
    }
    Serial.println("AUDIO-END");
}

void setup() {
    // Larger USB serial buffer so screenshot dumps arrive intact
    Serial.setTxBufferSize(4096);
    DEV_Module_Init(); // e-paper GPIO setup + Serial.begin(115200)
    // Wait (up to 3 s) for a USB serial monitor so the first lines aren't lost
    while (!Serial && millis() < 3000) delay(10);
    if (esp_reset_reason() == ESP_RST_POWERON) bootCount = lastUpMs = lastNapMs = lastNaps = lastSleepMs = 0;
    Serial.printf("zvukopis starting (boot %lu, reset reason %d)\n", ++bootCount, esp_reset_reason());
    if (lastUpMs > 0 && lastNapMs + lastSleepMs <= lastUpMs) {
        Serial.printf("[power] previous boot: up %lu s, of which %lu s in sleep mode and %lu s light-sleeping "
                      "between presses (%lu naps)\n",
                      lastUpMs / 1000, lastSleepMs / 1000, lastNapMs / 1000, lastNaps);
    }
    lastUpMs = lastNapMs = lastNaps = lastSleepMs = 0;
    setCpuFrequencyMhz(CPU_MHZ);

    boardPowerInit(); // also starts the I2C bus shared with the audio codec
    epdPowerOn();
    EPD_3IN97_Init();
    clockInit(); // before audio, so moments get start times

    if (!audioInit()) Serial.println("Audio init failed");
    uiStart();
    buttonsInit();
    Serial.println("Serial commands: u/d = up/down pressed, U/D = released, c = click, l = long click, "
                   "r = record start/stop, t = speaker/mic self-test, s = screenshot, p = PWR (sleep), T<unix time> = set clock, a = dump newest moment, "
                   "P = power stats, z = 3 s light sleep test (drops USB)");
}

void loop() {
    while (Serial.available()) {
        switch (Serial.read()) {
        case 'u': buttonsInject(BTN_UP); break;
        case 'd': buttonsInject(BTN_DOWN); break;
        case 'U': buttonsInject(BTN_UP_RELEASE); break;
        case 'D': buttonsInject(BTN_DOWN_RELEASE); break;
        case 'c': buttonsInject(BTN_CLICK); break;
        case 'l': buttonsInject(BTN_LONG_CLICK); break;
        case 'r':
            if (audioState() == AUDIO_RECORDING) audioStopRecording();
            else audioStartRecording();
            break;
        case 't': audioSelfTest(); break;
        case 's': uiRequestScreenshot(); break;
        case 'p': buttonsInject(BTN_POWER); break;
        case 'a': dumpNewestMoment(); break;
        case 'T': clockSet((time_t)Serial.parseInt()); break;
        case 'P': printPowerStats(); break;
        case 'z':
            Serial.println("[power] light sleep for 3 s, then USB reconnect");
            Serial.flush();
            nap(3000);
            usbReconnect();
            delay(1500);
            Serial.printf("[power] back after %lu ms nap\n", napTotalMs);
            break;
        }
    }

    ButtonEvent event;
    while (buttonsNextEvent(event)) uiHandleEvent(event);
    uiTick();
    if (uiTakeSleepRequest()) goToSleep();
    delay(10);
    napIfIdle();
}
