#pragma once

// Copy this file to wifi_config.h (which git ignores) and fill it in.
// Without wifi_config.h the clock still runs from the board's RTC, it just
// never gets corrected over the network.

#define WIFI_SSID "your network"
#define WIFI_PASSWORD "your password"

// POSIX TZ string for local time on screen and in file names.
// Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3", UK: "GMT0BST,M3.5.0/1,M10.5.0",
// US Eastern: "EST5EDT,M3.2.0,M11.1.0"
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"
