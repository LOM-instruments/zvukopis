#include "clock.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <esp_random.h>
#include <sys/time.h>

#include "audio.h"
#include "networks.h"

#if !defined(NO_WIFI_CONFIG) && __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef TIME_ZONE
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

static const uint8_t RTC_ADDRESS = 0x51;
static const uint8_t RTC_SECONDS_REG = 0x04; // then minutes, hours, days, weekdays, months, years
static const int MIN_VALID_YEAR = 2023, MAX_VALID_YEAR = 2037; // those of CLOCK_EARLIEST and CLOCK_LATEST

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
    if (t.tm_year + 1900 < MIN_VALID_YEAR || t.tm_year + 1900 > MAX_VALID_YEAR) return false;
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
    bool ok = join == WIFI_JOINED && clockNtp(shouldAbort);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!ok) {
        Serial.printf("[clock] sync %s after %lu ms\n", shouldAbort() ? "interrupted" : "failed", millis() - start);
        return join == WIFI_JOINED ? CLOCK_SYNC_FAILED : CLOCK_SYNC_NO_WIFI;
    }
    return CLOCK_SYNC_DONE;
}

static bool stopAsked(bool (*abort)()) {
    return syncPaused || (abort && abort());
}

// Whether `reply` is a time server's answer to the request that carried
// `sent` as its transmit time: a server sends that back as the origin time
static bool ntpAnswers(const uint8_t *reply, const uint8_t *sent) {
    static const uint8_t NO_TIME[8] = {};
    return (reply[0] & 0x07) == 4                  // mode: server
           && reply[1] >= 1 && reply[1] <= 15      // stratum: 0 is a refusal, 16 a server that has no time itself
           && memcmp(reply + 24, sent, 8) == 0     // origin time
           && memcmp(reply + 40, NO_TIME, 8) != 0; // transmit time
}

// One NTP exchange with `server`: the time, corrected for half the round trip.
// The port takes datagrams from anyone, so the request carries a random
// number where its own time would go, and only a reply that returns it
// counts; anything else is dropped and the wait goes on.
static bool ntpQuery(const char *server, timeval &out, bool (*abort)()) {
    IPAddress ip;
    if (!WiFi.hostByName(server, ip)) {
        Serial.printf("[clock] can't look up %s\n", server);
        return false;
    }
    WiFiUDP udp;
    udp.begin(0);
    for (int attempt = 0; attempt < 3 && !stopAsked(abort); attempt++) {
        uint8_t packet[48] = {0x1B}; // version 3, client
        esp_fill_random(packet + 40, 8);
        udp.beginPacket(ip, 123);
        udp.write(packet, sizeof(packet));
        udp.endPacket();
        int64_t sentUs = esp_timer_get_time();
        while (esp_timer_get_time() - sentUs < NTP_REPLY_MS * 1000LL && !stopAsked(abort)) {
            if (udp.parsePacket() <= 0) {
                delay(10);
                continue;
            }
            int64_t roundTripUs = esp_timer_get_time() - sentUs;
            uint8_t reply[48];
            bool whole = udp.read(reply, sizeof(reply)) == (int)sizeof(reply);
            udp.flush(); // what is left of a longer one: the next can't be read before this one is gone
            if (!whole || !ntpAnswers(reply, packet + 40)) continue;
            uint32_t seconds = (uint32_t)reply[40] << 24 | reply[41] << 16 | reply[42] << 8 | reply[43];
            uint32_t fraction = (uint32_t)reply[44] << 24 | reply[45] << 16 | reply[46] << 8 | reply[47];
            int64_t us = (int64_t)(seconds - NTP_UNIX_OFFSET) * 1000000 + ((uint64_t)fraction * 1000000 >> 32) +
                         roundTripUs / 2;
            out.tv_sec = us / 1000000;
            out.tv_usec = us % 1000000;
            udp.stop();
            Serial.printf("[clock] %s (%s) answered in %lld ms\n", server, ip.toString().c_str(), roundTripUs / 1000);
            if (out.tv_sec >= CLOCK_EARLIEST && out.tv_sec <= CLOCK_LATEST) return true;
            Serial.println("[clock] ... with a time the clock doesn't take");
            return false;
        }
    }
    udp.stop();
    if (!stopAsked(abort)) Serial.printf("[clock] no answer from %s (%s)\n", server, ip.toString().c_str());
    return false;
}

bool clockNtp(bool (*abort)()) {
    uint32_t start = millis();
    timeval tv;
    bool ok = false;
    for (const char *server : NTP_SERVERS) {
        if (stopAsked(abort) || (ok = ntpQuery(server, tv, abort))) break;
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

// The parts of a POSIX TZ rule. Each moves `p` past what it took.

static bool tzNumber(const char *&p, int min, int max) {
    int value = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 3) {
        value = value * 10 + (*p++ - '0');
        digits++;
    }
    return digits > 0 && !(*p >= '0' && *p <= '9') && value >= min && value <= max;
}

// A zone's short name: 3 to 10 letters, or between < and > also digits and signs
static bool tzName(const char *&p) {
    bool quoted = *p == '<';
    if (quoted) p++;
    int length = 0;
    for (;; p++, length++) {
        bool letter = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
        bool other = (*p >= '0' && *p <= '9') || *p == '+' || *p == '-';
        if (!letter && !(quoted && other)) break;
    }
    if (quoted && *p++ != '>') return false;
    return length >= 3 && length <= 10;
}

// h[:mm[:ss]]
static bool tzClock(const char *&p, int maxHours) {
    if (!tzNumber(p, 0, maxHours)) return false;
    for (int part = 0; part < 2 && *p == ':'; part++) {
        p++;
        if (!tzNumber(p, 0, 59)) return false;
    }
    return true;
}

// How far the zone is behind UTC: [+|-]h[:mm[:ss]]
static bool tzOffset(const char *&p) {
    if (*p == '+' || *p == '-') p++;
    return tzClock(p, 24);
}

// The day the clocks change, and when on that day: ,Mm.w.d or ,Jn or ,n, each with an optional /h[:mm[:ss]]
static bool tzChange(const char *&p) {
    if (*p++ != ',') return false;
    if (*p == 'M') {
        p++;
        if (!tzNumber(p, 1, 12) || *p++ != '.' || !tzNumber(p, 1, 5) || *p++ != '.' || !tzNumber(p, 0, 6)) return false;
    } else if (*p == 'J') {
        p++;
        if (!tzNumber(p, 1, 365)) return false;
    } else if (!tzNumber(p, 0, 365)) {
        return false;
    }
    if (*p != '/') return true;
    p++;
    return tzClock(p, 167);
}

// Whether `tz` is a POSIX TZ rule, such as "CET-1CEST,M3.5.0,M10.5.0/3",
// "<+0330>-3:30" or "UTC0". tzset() reports nothing: text it can't read (a
// zone's name like "Europe/Prague", a mistyped rule) leaves the old rule
// working until the next start, and from then on the clock shows UTC.
static bool isTimeZoneRule(const char *tz) {
    const char *p = tz;
    if (!tzName(p) || !tzOffset(p)) return false;
    if (*p == 0) return true; // a zone without daylight saving
    if (!tzName(p)) return false;
    if (*p != ',' && !tzOffset(p)) return false;
    // The two dates are asked for: tzset() would fill in those of the United States
    return tzChange(p) && tzChange(p) && *p == 0;
}

void clockInit() {
    char saved[sizeof(timeZone)] = "";
    Preferences prefs;
    prefs.begin("zvukopis", true);
    bool isSaved = prefs.isKey("tz") && prefs.getString("tz", saved, sizeof(saved)) > 0;
    prefs.end();
    if (isSaved && isTimeZoneRule(saved)) {
        strlcpy(timeZone, saved, sizeof(timeZone));
    } else if (isSaved) {
        // Saved by a version that took any text
        Serial.printf("[clock] the saved time zone \"%s\" is not a rule: back to %s\n", saved, timeZone);
        prefs.begin("zvukopis", false);
        prefs.remove("tz");
        prefs.end();
    }
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

bool clockSetTimeZone(const char *posixTz) {
    if (strlen(posixTz) >= sizeof(timeZone) || !isTimeZoneRule(posixTz)) return false;
    strlcpy(timeZone, posixTz, sizeof(timeZone));
    setenv("TZ", timeZone, 1);
    tzset();
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putString("tz", timeZone);
    prefs.end();
    Serial.printf("[clock] time zone %s\n", timeZone);
    return true;
}

bool clockValid() { return valid; }

bool clockSet(time_t utc) {
    if (utc < CLOCK_EARLIEST || utc > CLOCK_LATEST) return false;
    setSystemTime(utc);
    rtcWrite(utc);
    valid = true;
    char buf[32];
    tm local;
    localtime_r(&utc, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    Serial.printf("[clock] set to %s, zone %s\n", buf, timeZone);
    return true;
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
