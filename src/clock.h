#pragma once

#include <time.h>

// Wall clock. The board's PCF85063 RTC (I2C 0x51, same bus as the PMIC) keeps
// time in UTC; NTP over Wi-Fi corrects it when asked (Settings), and by itself
// only when the RTC has no valid time at startup. Local time follows a POSIX
// TZ rule set on the web interface (default: TIME_ZONE in wifi_config.h, or CET).
// Wi-Fi is only switched on for a sync, and never while recording, since the
// radio could add interference to the audio. Credentials and time zone come
// from src/wifi_config.h (see wifi_config.example.h).
// A time found in the RTC counts only if Zvukopis put it there: the factory
// firmware, for one, keeps local time in it.

// The clock takes no time outside this range: an earlier one is a clock that
// was never set, and the 32-bit time_t ends in January 2038
static const time_t CLOCK_EARLIEST = 1672531200; // 2023-01-01 00:00:00 UTC
static const time_t CLOCK_LATEST = 2145916799;   // 2037-12-31 23:59:59 UTC

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
// Sets the system clock and the RTC (serial 'T<unix time>', the web page).
// False, and nothing changes, for a time before 2023 or after 2037
// (CLOCK_EARLIEST, CLOCK_LATEST).
bool clockSet(time_t utc);

bool clockSyncAvailable(); // false while zvukopis knows no Wi-Fi network
const char *clockTimeZone(); // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
// Applies a POSIX TZ rule and keeps it. False, and nothing changes, when the
// text is not such a rule (a name like "Europe/Prague" is not).
bool clockSetTimeZone(const char *posixTz);
void clockSyncNow();       // starts a sync in the background
// State of the last sync, and when it started or finished (millis)
ClockSync clockSyncState(uint32_t &since);
bool clockSyncing();
// With Wi-Fi already connected: NTP time into the clock and RTC. `abort`, when
// given, is asked while it waits for an answer and ends the wait.
bool clockNtp(bool (*abort)() = nullptr);

// Sleep: stops a sync in progress (switching Wi-Fi off) and holds off new ones
void clockPauseSync(bool paused);
