#pragma once

// Copy this file to wifi_config.h (which git ignores) and fill it in.
// Without wifi_config.h the clock still runs from the board's RTC, it just
// never gets corrected over the network.

#define WIFI_SSID "your network"
#define WIFI_PASSWORD "your password"

// The web interface opens its own hotspot, named like the unit ("zvukopis-ab12"),
// when the network above isn't in reach. Each unit makes itself a password of
// 12 characters and shows it on its screen; a fixed one for the bench goes here
// (at least 8 characters).
// #define HOTSPOT_PASSWORD "only for the bench"

// POSIX TZ string for local time on screen and in file names.
// Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3", UK: "GMT0BST,M3.5.0/1,M10.5.0",
// US Eastern: "EST5EDT,M3.2.0,M11.1.0"
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"
