#pragma once

#include <time.h>

// Wall clock. The board's PCF85063 RTC (I2C 0x51, same bus as the PMIC) keeps
// time in UTC; NTP over Wi-Fi corrects it at startup and then once a day.
// Wi-Fi is only switched on for a sync, and never while recording, since the
// radio could add interference to the audio. Credentials and time zone come
// from src/wifi_config.h (see wifi_config.example.h).

// Requires Wire to be started (boardPowerInit does that). Sets the system
// clock from the RTC and starts the sync task.
void clockInit();

bool clockValid(); // false until the RTC or NTP has given a plausible time
void clockSet(time_t utc); // sets the system clock and the RTC (serial 'T<unix time>')

// Sleep: stops a sync in progress (switching Wi-Fi off) and holds off new ones
void clockPauseSync(bool paused);
