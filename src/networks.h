#pragma once

#include <stddef.h>

// The Wi-Fi networks zvukopis knows: up to four saved on the web interface (kept in
// NVS), plus the one in wifi_config.h if that file exists. And the password of
// its own hotspot, made up once per device unless wifi_config.h sets one.

static const int MAX_NETWORKS = 4;

struct WifiNetwork {
    char ssid[33];
    char password[65];
};

void wifiInit();
int wifiNetworkCount(); // saved on the web interface
bool wifiNetworkGet(int index, WifiNetwork &out);
bool wifiNetworkSave(const WifiNetwork &network); // replaces one with the same name; false when full
void wifiNetworkForget(int index);
bool wifiKnowsAny(); // saved ones, or wifi_config.h

enum WifiJoin {
    WIFI_JOINED,
    WIFI_NONE_KNOWN,    // nothing saved
    WIFI_NOT_IN_REACH,  // no known network in the scan
    WIFI_JOIN_FAILED,   // found, but couldn't connect (wrong password?)
};

// Scans and joins the strongest known network in reach, leaving Wi-Fi on when
// it succeeds and off otherwise. `abort` is checked while waiting.
WifiJoin wifiJoin(char *ssid, size_t len, bool (*abort)() = nullptr);

const char *wifiHotspotName();
// This unit's own name, "zvukopis-ab12" (the last two bytes of its factory
// address): its host name on a network, the name the page answers to
// (http://<name>.local), and the name of its hotspot, so that several units in
// one place can be told apart
const char *wifiHostName();
const char *wifiHotspotPassword();
