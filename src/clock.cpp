#include "clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <sys/time.h>

#include "audio.h"
#include "networks.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef TIME_ZONE
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

static const uint8_t RTC_ADDRESS = 0x51;
static const uint8_t RTC_SECONDS_REG = 0x04; // then minutes, hours, days, weekdays, months, years
static const int MIN_VALID_YEAR = 2025;

static const uint32_t NTP_REPLY_MS = 1500; // per request; three per server
static const uint32_t NTP_UNIX_OFFSET = 2208988800UL; // 1900 to 1970
static const char *NTP_SERVERS[] = {"pool.ntp.org", "time.google.com"};

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

static ClockSync syncOnce() {
    uint32_t start = millis();
    char ssid[33];
    WifiJoin join = wifiJoin(ssid, sizeof(ssid), shouldAbort);
    bool ok = join == WIFI_JOINED && clockNtp();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!ok) {
        Serial.printf("[clock] sync %s after %lu ms\n", shouldAbort() ? "interrupted" : "failed", millis() - start);
        return join == WIFI_JOINED ? CLOCK_SYNC_FAILED : CLOCK_SYNC_NO_WIFI;
    }
    return CLOCK_SYNC_DONE;
}

// One NTP exchange with `server`: the time, corrected for half the round trip
static bool ntpQuery(const char *server, timeval &out) {
    IPAddress ip;
    if (!WiFi.hostByName(server, ip)) {
        Serial.printf("[clock] can't look up %s\n", server);
        return false;
    }
    WiFiUDP udp;
    udp.begin(0);
    for (int attempt = 0; attempt < 3 && !syncPaused; attempt++) {
        uint8_t packet[48] = {0x1B}; // version 3, client
        udp.beginPacket(ip, 123);
        udp.write(packet, sizeof(packet));
        udp.endPacket();
        int64_t sentUs = esp_timer_get_time();
        while (esp_timer_get_time() - sentUs < NTP_REPLY_MS * 1000LL) {
            if (udp.parsePacket() >= 48 && udp.read(packet, sizeof(packet)) == 48) {
                int64_t roundTripUs = esp_timer_get_time() - sentUs;
                uint32_t seconds = (uint32_t)packet[40] << 24 | packet[41] << 16 | packet[42] << 8 | packet[43];
                uint32_t fraction = (uint32_t)packet[44] << 24 | packet[45] << 16 | packet[46] << 8 | packet[47];
                int64_t us = (int64_t)(seconds - NTP_UNIX_OFFSET) * 1000000 + ((uint64_t)fraction * 1000000 >> 32) +
                             roundTripUs / 2;
                out.tv_sec = us / 1000000;
                out.tv_usec = us % 1000000;
                udp.stop();
                Serial.printf("[clock] %s (%s) answered in %lld ms\n", server, ip.toString().c_str(), roundTripUs / 1000);
                return seconds > NTP_UNIX_OFFSET;
            }
            delay(10);
        }
    }
    udp.stop();
    Serial.printf("[clock] no answer from %s (%s)\n", server, ip.toString().c_str());
    return false;
}

bool clockNtp() {
    uint32_t start = millis();
    timeval tv;
    bool ok = false;
    for (const char *server : NTP_SERVERS) {
        if ((ok = ntpQuery(server, tv)) || syncPaused) break;
    }
    if (!ok) return false;
    time_t rtc;
    bool hadRtc = rtcRead(rtc);
    settimeofday(&tv, nullptr);

    time_t now = tv.tv_sec;
    rtcWrite(now);
    valid = true;
    char buf[32];
    tm local;
    localtime_r(&now, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    if (hadRtc) Serial.printf("[clock] NTP time in %lu ms: %s (RTC was off by %ld s)\n", millis() - start, buf, (long)(rtc - now));
    else Serial.printf("[clock] NTP time in %lu ms: %s\n", millis() - start, buf);
    return true;
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

static char timeZone[64] = TIME_ZONE;

void clockInit() {
    Preferences prefs;
    prefs.begin("zvukopis", true);
    if (prefs.isKey("tz")) prefs.getString("tz", timeZone, sizeof(timeZone));
    prefs.end();
    setenv("TZ", timeZone, 1);
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
    xTaskCreatePinnedToCore(syncTask, "clock", 4096, NULL, 1, &syncTaskHandle, 0);
    if (!valid && wifiKnowsAny()) clockSyncNow();
}

const char *clockTimeZone() { return timeZone; }

void clockSetTimeZone(const char *posixTz) {
    strlcpy(timeZone, posixTz, sizeof(timeZone));
    setenv("TZ", timeZone, 1);
    tzset();
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putString("tz", timeZone);
    prefs.end();
    Serial.printf("[clock] time zone %s\n", timeZone);
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

bool clockSyncAvailable() { return wifiKnowsAny(); }

void clockSyncNow() {
    if (!syncTaskHandle || syncing || !wifiKnowsAny()) return;
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
