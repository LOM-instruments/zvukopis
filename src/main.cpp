#include <Arduino.h>
#include <esp_sleep.h>
#include <mbedtls/base64.h>

#include "EPD_3in97.h"

#include "audio.h"
#include "board_power.h"
#include "buttons.h"
#include "clock.h"
#include "ui.h"

// How often the chip checks the PWR key while "asleep" on USB power
static const uint32_t USB_POLL_MS = 200;

// Light sleep keeps PSRAM powered, so memos held in memory survive it.
// A PWR press pulls the PMIC's IRQ line low, which wakes the chip.
// On USB power the chip stays awake instead: light sleep shuts down its USB
// port, which doesn't come back afterwards, and power isn't scarce then.
// A 'p' over serial also wakes it in that case.
static bool sleepUntilPowerKey() {
    if (usbPowered()) {
        while (Serial.available()) Serial.read();
        for (;;) {
            delay(USB_POLL_MS);
            if (powerKeyPressed()) return true;
            while (Serial.available()) {
                if (Serial.read() == 'p') return true;
            }
            if (!usbPowered()) return false; // unplugged: continue in light sleep
        }
    }
    powerKeyWakeup(true);
    do {
        esp_light_sleep_start();
    } while (!powerKeyPressed());
    powerKeyWakeup(false);
    return true;
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
        Serial.println("[sleep] USB unplugged, switching to light sleep");
    }

    Serial.println("[sleep] woke up");
    audioResume();
    uiShowSleepScreen(false);
    buttonsPause(false);
    clockPauseSync(false);
}

// Debug: newest memo's raw samples as base64, for analysis on a computer
static void dumpNewestMemo() {
    Memo memo;
    if (!memoGet(0, memo) || !memo.samples) {
        Serial.println("No memo in memory to dump");
        return;
    }
    Serial.printf("AUDIO-BEGIN memo=%d samples=%u\n", memo.number, memo.length);
    const uint8_t *bytes = (const uint8_t *)memo.samples;
    size_t total = memo.length * sizeof(int16_t);
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
    Serial.println("zvukopis starting");

    boardPowerInit(); // also starts the I2C bus shared with the audio codec
    epdPowerOn();
    EPD_3IN97_Init();
    clockInit(); // before audio, so memos get start times

    if (!audioInit()) Serial.println("Audio init failed");
    uiStart();
    buttonsInit();
    Serial.println("Serial commands: u/d = up/down, c = click, l = long click, "
                   "r = record start/stop, t = speaker/mic self-test, s = screenshot, p = PWR (sleep), T<unix time> = set clock, a = dump newest memo");
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
        case 'a': dumpNewestMemo(); break;
        case 'T': clockSet((time_t)Serial.parseInt()); break;
        }
    }

    ButtonEvent event;
    while (buttonsNextEvent(event)) uiHandleEvent(event);
    uiTick();
    if (uiTakeSleepRequest()) goToSleep();
    delay(10);
}
