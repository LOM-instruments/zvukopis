#include <Arduino.h>

#include "EPD_3in97.h"

#include "audio.h"
#include "board_power.h"
#include "buttons.h"
#include "ui.h"

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
                   "r = record start/stop, t = speaker/mic self-test, s = screenshot");
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
        }
    }

    ButtonEvent event;
    while (buttonsNextEvent(event)) uiHandleEvent(event);
    uiTick();
    delay(10);
}
