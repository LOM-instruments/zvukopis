#include <Arduino.h>
#include <esp_sleep.h>
#include <mbedtls/base64.h>
#include <soc/usb_serial_jtag_reg.h>
#include <sys/time.h>

#include "EPD_3in97.h"

#include "audio.h"
#include "board_power.h"
#include "buttons.h"
#include "clock.h"
#include "config.h"
#include "library.h"
#include "networks.h"
#include "storage.h"
#include "ui.h"

// How often the chip checks the PWR key while "asleep" on USB power
static const uint32_t USB_POLL_MS = 50;
static const uint32_t QUICK_RECORD_HOLD_MS = 800; // the rocker held in while asleep: record
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

// --- Scheduled recordings (set up on the web interface) ---

static const uint32_t SCHEDULE_LEAD_MS = 300; // wake this early, so recording starts on the second
static int64_t scheduledStart = 0; // start of the entry being recorded, 0 if none
static uint32_t scheduledAt = 0;   // millis() when its recording was requested

// Until the next scheduled recording should start (minus the lead), or UINT32_MAX
static uint32_t msUntilSchedule() {
    if (!clockValid()) return UINT32_MAX;
    timeval tv;
    gettimeofday(&tv, nullptr);
    Schedule s;
    int index;
    if (!scheduleNext(tv.tv_sec, s, index)) return UINT32_MAX;
    int64_t ms = (s.start - tv.tv_sec) * 1000 - tv.tv_usec / 1000 - SCHEDULE_LEAD_MS;
    return ms <= 0 ? 0 : (uint32_t)min(ms, (int64_t)UINT32_MAX - 1);
}

// Starts a due recording and marks it done once it has run; true while one runs
static bool scheduleTick() {
    if (!clockValid()) return false;
    time_t now = time(nullptr);
    if (scheduledStart) {
        // Right after the request the audio task may not have started yet
        AudioState a = audioState();
        if (a == AUDIO_RECORDING || a == AUDIO_SAVING || millis() - scheduledAt < 1000) return true;
        scheduleDone(scheduledStart, now);
        scheduledStart = 0;
        return false;
    }
    // Entries whose time passed unrecorded (the board was off, say) are dropped,
    // or moved on a day when they repeat
    Schedule s;
    while (scheduleGet(0, s) && s.start + s.seconds <= now) {
        Serial.println("[schedule] missed one");
        scheduleDone(s.start, now);
    }
    int index;
    if (!scheduleNext(now, s, index) || now < s.start || !uiCanRecord()) return false;
    AudioState a = audioState();
    if (a == AUDIO_RECORDING || a == AUDIO_SAVING || a == AUDIO_FORMATTING) return false; // tried again while its time lasts
    if (a == AUDIO_PLAYING) audioStopPlayback();
    uint32_t seconds = (uint32_t)(s.start + s.seconds - now);
    Serial.printf("[schedule] recording for %lu s\n", seconds);
    audioStartRecording(seconds);
    scheduledStart = s.start;
    scheduledAt = millis();
    return true;
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
    uint32_t ms = min(uiMsUntilNextChange(), msUntilSchedule());
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
// being plugged in, a timer for a scheduled recording, and one once a minute
// in case a USB event was missed.
// On USB power the chip stays awake instead, so its serial port keeps working;
// power isn't scarce then. A 'p' over serial also wakes it in that case.
static const uint32_t SLEEP_RECHECK_MS = 60 * 1000;

enum Wake { WAKE_KEY, WAKE_POWER_SOURCE, WAKE_SCHEDULE, WAKE_QUICK_RECORD };

// The rocker was pressed in while asleep: held for the hold time, it's a
// quick record; released before that, a knock.
static uint32_t fakeClickUntil = 0; // serial 'q'/'Q' while asleep: pretend the rocker is held
static bool clickHeld() {
    if (fakeClickUntil && (int32_t)(fakeClickUntil - millis()) <= 0) fakeClickUntil = 0;
    return buttonsClickHeld() || fakeClickUntil != 0;
}

static bool quickPressHeld() {
    uint32_t pressed = millis();
    while (clickHeld()) {
        if (millis() - pressed >= QUICK_RECORD_HOLD_MS) {
            Serial.println("[sleep] rocker held: quick record");
            return true;
        }
        delay(10);
    }
    return false; // a knock
}

static Wake sleepUntilWake() {
    if (usbPowered()) {
        while (Serial.available()) Serial.read();
        for (;;) {
            delay(USB_POLL_MS);
            if (powerKeyPressed()) return WAKE_KEY;
            if (clickHeld() && quickPressHeld()) return WAKE_QUICK_RECORD;
            while (Serial.available()) {
                int c = Serial.read();
                if (c == 'p') return WAKE_KEY;
                if (c == 's') uiRequestScreenshot(); // of the sleep screen
                if (c == 'q') fakeClickUntil = millis() + 1500; // a hold: quick record
                if (c == 'Q') fakeClickUntil = millis() + 300;  // a tap: dropped
            }
            if (msUntilSchedule() == 0) return WAKE_SCHEDULE;
            if (!usbPowered()) return WAKE_POWER_SOURCE; // unplugged: continue in light sleep
        }
    }
    powerKeyWakeup(true);
    buttonsClickWakeup(true);
    uint32_t start = millis();
    Wake wake;
    for (;;) {
        uint32_t ms = min(msUntilSchedule(), SLEEP_RECHECK_MS);
        if (ms == 0) {
            wake = WAKE_SCHEDULE;
            break;
        }
        esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
        esp_light_sleep_start();
        if (powerKeyPressed()) {
            wake = WAKE_KEY;
            break;
        }
        if (buttonsClickHeld()) {
            if (quickPressHeld()) {
                wake = WAKE_QUICK_RECORD;
                break;
            }
            continue;
        }
        if (usbPowered()) {
            wake = WAKE_POWER_SOURCE;
            break;
        }
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    buttonsClickWakeup(false);
    powerKeyWakeup(false);
    sleepTotalMs += millis() - start;
    lastUpMs = millis();
    lastSleepMs = sleepTotalMs;
    if (usbPowered()) usbReconnect(); // plugged in while asleep
    return wake;
}

// A scheduled recording while asleep. The screen is left as it is: a refresh
// would add noise to the start. Returns true if PWR was held meanwhile, which
// stops the recording and wakes zvukopis.
static bool recordWhileAsleep() {
    Serial.println("[sleep] scheduled recording");
    bool started = false, woken = false;
    for (;;) {
        bool running = scheduleTick();
        if (running) started = true;
        else if (started || msUntilSchedule() > 0) break; // done, or nothing due after all
        if (!woken && powerKeyPressed()) {
            woken = true;
            audioStopRecording();
        }
        delay(20);
    }
    uint32_t start = millis();
    while (!audioAsleep() && millis() - start < 5000) delay(50); // light sleep needs the codec off
    return woken;
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
    bool quickRecord = false;
    for (;;) {
        Wake wake = sleepUntilWake();
        if (wake == WAKE_KEY) break;
        if (wake == WAKE_QUICK_RECORD) {
            quickRecord = true;
            break;
        }
        if (wake == WAKE_SCHEDULE && recordWhileAsleep()) break;
        if (wake == WAKE_POWER_SOURCE) Serial.printf("[sleep] now on %s\n", usbPowered() ? "USB" : "battery");
    }

    if (quickRecord) {
        // The recording screen first, so the panel refresh (which the mic
        // hears) is over before the recording starts
        uint32_t start = millis();
        uiQuickRecordScreen();
        audioStartRecording();
        // Keep that screen until the audio task is recording, or the main
        // screen would show for a moment in between
        uint32_t t = millis();
        while (audioState() != AUDIO_RECORDING && millis() - t < 2000) delay(10);
        uiQuickRecordStarted();
        Serial.printf("[sleep] quick record: screen in %lu ms, recording\n", millis() - start);
    }
    Serial.println("[sleep] woke up");
    uiShowSleepScreen(false);
    buttonsPause(false); // the rocker still held from the wake-up press is not a click
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
    wifiInit();  // before the clock, which may sync at startup
    clockInit(); // before audio, so moments get start times
    configInit();

    if (!audioInit()) Serial.println("Audio init failed");
    libraryScan(); // the card is mounted by audioInit
    uiStart();
    buttonsInit();
    Serial.println("Serial commands: u/d = up/down pressed, U/D = released, c = click, l = long click, m = BOOT (menu), "
                   "r = record start/stop, t = speaker/mic self-test, s = screenshot, p = PWR (sleep), T<unix time> = set clock, a = dump newest moment, f = list the card's files, "
                   "P = power stats, z = 3 s light sleep test (drops USB); while asleep: q/Q = rocker held 1.5 s / tapped");
}

void loop() {
    while (Serial.available()) {
        switch (Serial.read()) {
        case 'u': buttonsInject(BTN_UP); break;
        case 'd': buttonsInject(BTN_DOWN); break;
        case 'U': buttonsInject(BTN_UP_RELEASE); break;
        case 'm': buttonsInject(BTN_MENU); break;
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
        case 'f': storageListFiles(); break;
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
    scheduleTick();
    if (uiTakeSleepRequest()) goToSleep();
    delay(10);
    napIfIdle();
}
