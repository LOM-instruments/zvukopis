#include "clock.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "audio.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef TIME_ZONE
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

static const uint8_t RTC_ADDRESS = 0x51;
static const uint8_t RTC_SECONDS_REG = 0x04; // then minutes, hours, days, weekdays, months, years
static const int MIN_VALID_YEAR = 2025;

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t NTP_TIMEOUT_MS = 10000;

static volatile bool valid = false;
static volatile bool syncPaused = false;
static volatile bool syncing = false;
static volatile ClockSync syncState = CLOCK_SYNC_NONE;
static volatile uint32_t syncStateSince = 0;
static TaskHandle_t syncTaskHandle = nullptr;

static uint8_t fromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t toBcd(int v) { return ((v / 10) << 4) | (v % 10); }

// UTC broken-down time to time_t, without touching the TZ setting
static time_t utcToTime(const tm &t) {
    int y = t.tm_year + 1900, m = t.tm_mon + 1;
    if (m <= 2) y--;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

// False when the RTC can't be read or its oscillator stopped (time lost)
static bool rtcRead(time_t &out) {
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(RTC_SECONDS_REG);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(RTC_ADDRESS, (uint8_t)7) != 7) return false;
    uint8_t r[7];
    for (uint8_t &b : r) b = Wire.read();
    if (r[0] & 0x80) return false;

    tm t = {};
    t.tm_sec = fromBcd(r[0] & 0x7F);
    t.tm_min = fromBcd(r[1] & 0x7F);
    t.tm_hour = fromBcd(r[2] & 0x3F);
    t.tm_mday = fromBcd(r[3] & 0x3F);
    t.tm_mon = fromBcd(r[5] & 0x1F) - 1;
    t.tm_year = fromBcd(r[6]) + 100;
    if (t.tm_year + 1900 < MIN_VALID_YEAR) return false;
    out = utcToTime(t);
    return true;
}

static bool rtcWrite(time_t now) {
    tm t;
    gmtime_r(&now, &t);
    Wire.beginTransmission(RTC_ADDRESS);
    Wire.write(RTC_SECONDS_REG);
    Wire.write(toBcd(t.tm_sec)); // also clears the oscillator-stopped flag
    Wire.write(toBcd(t.tm_min));
    Wire.write(toBcd(t.tm_hour));
    Wire.write(toBcd(t.tm_mday));
    Wire.write(t.tm_wday);
    Wire.write(toBcd(t.tm_mon + 1));
    Wire.write(toBcd(t.tm_year - 100));
    return Wire.endTransmission() == 0;
}

static void setSystemTime(time_t now) {
    timeval tv = {now, 0};
    settimeofday(&tv, nullptr);
}

static bool shouldAbort() {
    return syncPaused || audioState() == AUDIO_RECORDING;
}

#ifdef WIFI_SSID
static bool waitFor(bool (*done)(), uint32_t timeoutMs) {
    uint32_t start = millis();
    while (!done()) {
        if (shouldAbort() || millis() - start > timeoutMs) return false;
        delay(100);
    }
    return true;
}

static ClockSync syncOnce() {
    uint32_t start = millis();
    Serial.printf("[clock] connecting to %s for NTP\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    bool connected = waitFor([] { return WiFi.status() == WL_CONNECTED; }, CONNECT_TIMEOUT_MS);
    bool ok = connected;
    if (connected) {
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
        configTzTime(TIME_ZONE, "pool.ntp.org", "time.google.com");
        ok = waitFor([] { return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED; }, NTP_TIMEOUT_MS);
        sntp_stop();
    }
    int status = WiFi.status(); // 1 = network not found, 4 = connect failed (password?), 6 = no answer
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!ok) {
        Serial.printf("[clock] sync %s after %lu ms (Wi-Fi status %d)\n",
                      shouldAbort() ? "interrupted" : "failed", millis() - start, status);
        return connected ? CLOCK_SYNC_FAILED : CLOCK_SYNC_NO_WIFI;
    }
    time_t now = time(nullptr), rtc;
    bool hadRtc = rtcRead(rtc);
    rtcWrite(now);
    valid = true;
    char buf[32];
    tm local;
    localtime_r(&now, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    if (hadRtc) Serial.printf("[clock] synced in %lu ms: %s (RTC was off by %ld s)\n", millis() - start, buf, (long)(rtc - now));
    else Serial.printf("[clock] synced in %lu ms: %s\n", millis() - start, buf);
    return CLOCK_SYNC_DONE;
}

// Waits for clockSyncNow()
static void syncTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (shouldAbort()) {
            syncState = CLOCK_SYNC_NONE;
            syncing = false;
            continue;
        }
        ClockSync result = syncOnce();
        syncState = result;
        syncStateSince = millis();
        syncing = false;
    }
}
#endif

void clockInit() {
    setenv("TZ", TIME_ZONE, 1);
    tzset();
    time_t now;
    if (rtcRead(now)) {
        setSystemTime(now);
        valid = true;
        char buf[32];
        tm local;
        localtime_r(&now, &local);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
        Serial.printf("[clock] RTC time %s\n", buf);
    } else {
        Serial.println("[clock] RTC has no valid time");
    }
#ifdef WIFI_SSID
    xTaskCreatePinnedToCore(syncTask, "clock", 4096, NULL, 1, &syncTaskHandle, 0);
    if (!valid) clockSyncNow();
#else
    Serial.println("[clock] no src/wifi_config.h, so no NTP sync");
#endif
}

bool clockValid() { return valid; }

void clockSet(time_t utc) {
    setSystemTime(utc);
    rtcWrite(utc);
    valid = true;
    char buf[32];
    tm local;
    localtime_r(&utc, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    Serial.printf("[clock] set to %s\n", buf);
}

bool clockSyncAvailable() { return syncTaskHandle != nullptr; }

void clockSyncNow() {
    if (!syncTaskHandle || syncing) return;
    syncing = true; // shown right away; cleared by the sync task
    syncState = CLOCK_SYNCING;
    syncStateSince = millis();
    xTaskNotifyGive(syncTaskHandle);
}

bool clockSyncing() { return syncing; }

ClockSync clockSyncState(uint32_t &since) {
    since = syncStateSince;
    return syncState;
}

void clockPauseSync(bool paused) {
    syncPaused = paused;
    if (!paused) return;
    uint32_t start = millis();
    while (syncing && millis() - start < 3000) delay(20);
}
