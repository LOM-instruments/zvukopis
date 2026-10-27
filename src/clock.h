#pragma once

#include <time.h>

// Wall clock. The board's PCF85063 RTC (I2C 0x51, same bus as the PMIC) keeps
// time in UTC; NTP over Wi-Fi corrects it when asked (Settings), and by itself
// only when the RTC has no valid time at startup. Local time follows a POSIX
// TZ rule set on the web page (default: TIME_ZONE in wifi_config.h, or CET).
// Wi-Fi is only switched on for a sync, and never while recording, since the
// radio could add interference to the audio. Credentials and time zone come
// from src/wifi_config.h (see wifi_config.example.h).

enum ClockSync {
    CLOCK_SYNC_NONE,
    CLOCK_SYNCING,
    CLOCK_SYNC_DONE,
    CLOCK_SYNC_NO_WIFI, // couldn't connect to the network
    CLOCK_SYNC_FAILED,  // connected, but no answer from the time servers
};

// Requires Wire to be started (boardPowerInit does that). Sets the system
// clock from the RTC and starts the sync task.
void clockInit();

bool clockValid(); // false until the RTC or NTP has given a plausible time
void clockSet(time_t utc); // sets the system clock and the RTC (serial 'T<unix time>')

bool clockSyncAvailable(); // false while zvukopis knows no Wi-Fi network
const char *clockTimeZone(); // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
void clockSetTimeZone(const char *posixTz); // applies it and keeps it
void clockSyncNow();       // starts a sync in the background
// State of the last sync, and when it started or finished (millis)
ClockSync clockSyncState(uint32_t &since);
bool clockSyncing();
bool clockNtp(); // with Wi-Fi already connected: NTP time into the clock and RTC

// Sleep: stops a sync in progress (switching Wi-Fi off) and holds off new ones
void clockPauseSync(bool paused);
