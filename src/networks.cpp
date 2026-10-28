#include "networks.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#if !defined(NO_WIFI_CONFIG) && __has_include("wifi_config.h")
#include "wifi_config.h"
#endif

static const char *HOTSPOT_NAME = "zvukopis";
static const char *HOST_NAME = "zvukopis";
static const uint32_t CONNECT_TIMEOUT_MS = 15000;
static const int HOTSPOT_PASSWORD_LEN = 8;

static SemaphoreHandle_t lock;
static WifiNetwork networks[MAX_NETWORKS];
static int networkTotal = 0;
static char hotspotPassword[17];

static void saveNetworks() {
    Preferences prefs;
    prefs.begin("zvukopis", false);
    // Preferences ignores a zero-length write, which would bring forgotten networks back
    if (networkTotal > 0) prefs.putBytes("networks", networks, networkTotal * sizeof(WifiNetwork));
    else prefs.remove("networks");
    prefs.end();
}

void wifiInit() {
    lock = xSemaphoreCreateMutex();
    Preferences prefs;
    prefs.begin("zvukopis", false);
    size_t len = prefs.isKey("networks") ? prefs.getBytesLength("networks") : 0;
    if (len % sizeof(WifiNetwork) == 0 && len <= sizeof(networks)) {
        networkTotal = prefs.getBytes("networks", networks, len) / sizeof(WifiNetwork);
    }
#ifdef HOTSPOT_PASSWORD
    strlcpy(hotspotPassword, HOTSPOT_PASSWORD, sizeof(hotspotPassword));
#else
    // Easy to type: no 0/o, 1/l/i
    if (prefs.getString("hotspotpass", hotspotPassword, sizeof(hotspotPassword)) == 0) {
        const char *alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
        for (int i = 0; i < HOTSPOT_PASSWORD_LEN; i++) hotspotPassword[i] = alphabet[esp_random() % strlen(alphabet)];
        hotspotPassword[HOTSPOT_PASSWORD_LEN] = 0;
        prefs.putString("hotspotpass", hotspotPassword);
    }
#endif
    prefs.end();
    Serial.printf("[wifi] %d saved network%s%s\n", networkTotal, networkTotal == 1 ? "" : "s",
#ifdef WIFI_SSID
                  ", plus wifi_config.h"
#else
                  ""
#endif
    );
}

int wifiNetworkCount() { return networkTotal; }

bool wifiNetworkGet(int index, WifiNetwork &out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = index >= 0 && index < networkTotal;
    if (ok) out = networks[index];
    xSemaphoreGive(lock);
    return ok;
}

bool wifiNetworkSave(const WifiNetwork &network) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int i = 0;
    while (i < networkTotal && strcmp(networks[i].ssid, network.ssid) != 0) i++;
    bool ok = i < MAX_NETWORKS;
    if (ok) {
        networks[i] = network;
        if (i == networkTotal) networkTotal++;
        saveNetworks();
    }
    xSemaphoreGive(lock);
    return ok;
}

void wifiNetworkForget(int index) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (index >= 0 && index < networkTotal) {
        memmove(&networks[index], &networks[index + 1], (networkTotal - index - 1) * sizeof(WifiNetwork));
        networkTotal--;
        saveNetworks();
    }
    xSemaphoreGive(lock);
}

bool wifiKnowsAny() {
#ifdef WIFI_SSID
    return true;
#else
    return networkTotal > 0;
#endif
}

// The known network with this name, from the saved ones or wifi_config.h
static bool knownNetwork(const String &ssid, WifiNetwork &out) {
    for (int i = 0; wifiNetworkGet(i, out); i++) {
        if (ssid == out.ssid) return true;
    }
#ifdef WIFI_SSID
    if (ssid == WIFI_SSID) {
        strlcpy(out.ssid, WIFI_SSID, sizeof(out.ssid));
        strlcpy(out.password, WIFI_PASSWORD, sizeof(out.password));
        return true;
    }
#endif
    return false;
}

WifiJoin wifiJoin(char *ssid, size_t len, bool (*abort)()) {
    if (!wifiKnowsAny()) return WIFI_NONE_KNOWN;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOST_NAME);
    int found = WiFi.scanNetworks();
    WifiNetwork best = {};
    int bestRssi = -1000;
    for (int i = 0; i < found; i++) {
        WifiNetwork known;
        if (WiFi.RSSI(i) > bestRssi && knownNetwork(WiFi.SSID(i), known)) {
            best = known;
            bestRssi = WiFi.RSSI(i);
        }
    }
    WiFi.scanDelete();
    if (bestRssi == -1000) {
        Serial.printf("[wifi] none of the known networks in reach (%d seen)\n", found);
        WiFi.mode(WIFI_OFF);
        return WIFI_NOT_IN_REACH;
    }

    Serial.printf("[wifi] joining %s (%d dBm)\n", best.ssid, bestRssi);
    WiFi.begin(best.ssid, best.password);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((abort && abort()) || millis() - start > CONNECT_TIMEOUT_MS) {
            Serial.printf("[wifi] couldn't join %s (status %d)\n", best.ssid, WiFi.status());
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return WIFI_JOIN_FAILED;
        }
        delay(100);
    }
    strlcpy(ssid, best.ssid, len);
    return WIFI_JOINED;
}

const char *wifiHotspotName() { return HOTSPOT_NAME; }
const char *wifiHotspotPassword() { return hotspotPassword; }
