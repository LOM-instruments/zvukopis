#include <Arduino.h>
#include <esp_sleep.h>

#include "EPD_3in97.h"

#include "audio.h"
#include "board_power.h"
#include "buttons.h"
#include "ui.h"

// How often the sleeping chip wakes briefly to check the PWR key
static const uint64_t POWER_KEY_POLL_US = 200 * 1000;

// Light sleep keeps PSRAM powered, so memos held in memory survive it.
// The PWR key reaches the chip only through the PMIC over I2C, so instead of a
// wake-up pin, a timer wakes it every 200 ms to ask the PMIC.
// On USB power the chip stays awake instead: light sleep shuts down its USB
// port, which doesn't come back afterwards, and power isn't scarce then.
// A 'p' over serial also wakes it in that case.
static bool sleepUntilPowerKey() {
    if (usbPowered()) {
        while (Serial.available()) Serial.read();
        for (;;) {
            delay(POWER_KEY_POLL_US / 1000);
            if (powerKeyPressed()) return true;
            while (Serial.available()) {
                if (Serial.read() == 'p') return true;
            }
            if (!usbPowered()) return false; // unplugged: continue in light sleep
        }
    }
    esp_sleep_enable_timer_wakeup(POWER_KEY_POLL_US);
    do {
        esp_light_sleep_start();
    } while (!powerKeyPressed());
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    return true;
}

static void goToSleep() {
    Serial.println("[sleep] going to sleep");
    audioSuspend(); // saves a recording in progress first
    buttonsPause(true);
    uiShowSleepScreen(true);
    Serial.println("[sleep] asleep");
    Serial.flush();

    powerKeyPressed(); // drop a press made while the sleep screen was drawn
    while (!sleepUntilPowerKey()) {
        Serial.println("[sleep] USB unplugged, switching to light sleep");
    }

    Serial.println("[sleep] woke up");
    audioResume();
    uiShowSleepScreen(false);
    buttonsPause(false);
}

void setup() {
    // Larger USB serial buffer so screenshot dumps arrive intact
    Serial.setTxBufferSize(4096);
    DEV_Module_Init(); // e-paper GPIO setup + Serial.begin(115200)
    // Wait (up to 3 s) for a USB serial monitor so the first lines aren't lost
    while (!Serial && millis() < 3000) delay(10);
    Serial.println("Voice memos starting");

    boardPowerInit(); // also starts the I2C bus shared with the audio codec
    epdPowerOn();
    EPD_3IN97_Init();

    if (!audioInit()) Serial.println("Audio init failed");
    uiStart();
    buttonsInit();
    Serial.println("Serial commands: u/d = up/down, c = click, l = long click, "
                   "r = record start/stop, t = speaker/mic self-test, s = screenshot, p = PWR (sleep)");
}

void loop() {
    while (Serial.available()) {
        switch (Serial.read()) {
        case 'u': buttonsInject(BTN_UP); break;
        case 'd': buttonsInject(BTN_DOWN); break;
        case 'c': buttonsInject(BTN_CLICK); break;
        case 'l': buttonsInject(BTN_LONG_CLICK); break;
        case 'r':
            if (audioState() == AUDIO_RECORDING) audioStopRecording();
            else audioStartRecording();
            break;
        case 't': audioSelfTest(); break;
        case 's': uiRequestScreenshot(); break;
        case 'p': buttonsInject(BTN_POWER); break;
        }
    }

    ButtonEvent event;
    while (buttonsNextEvent(event)) uiHandleEvent(event);
    uiTick();
    if (uiTakeSleepRequest()) goToSleep();
    delay(10);
}
