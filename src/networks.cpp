#include "networks.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <nvs.h>

#if !defined(NO_WIFI_CONFIG) && __has_include("wifi_config.h")
#include "wifi_config.h"
#endif

static const uint32_t CONNECT_TIMEOUT_MS = 15000;
// Twelve characters of 31: a recorded join to the hotspot cannot be tried out
// against every password in any useful time (eight took a graphics card days)
static const size_t HOTSPOT_PASSWORD_LEN = 12;

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

// Releases up to 2026-11-06 let the Wi-Fi driver keep the last network joined,
// password included, in a store of its own, which Forget on the page never
// reached. Emptied once. It is a namespace apart from this firmware's settings
// ("zvukopis") and from the radio's calibration ("phy"), and the driver, finding
// it empty, starts from its defaults as on a new unit. (The settings store marks
// entries as erased and overwrites them when their page is recycled; only Reset
// settings wipes the flash itself.)
static void forgetDriverCopy(Preferences &prefs) {
    if (prefs.getBool("wifiram", false)) return;
    nvs_handle_t driver;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READWRITE, &driver);
    if (err == ESP_OK) {
        err = nvs_erase_all(driver);
        if (err == ESP_OK) err = nvs_commit(driver);
        nvs_close(driver);
    }
    if (err != ESP_OK) return; // tried again at the next start
    prefs.putBool("wifiram", true);
    Serial.println("[wifi] the driver's own stored copy of the last network removed");
}

void wifiInit() {
    lock = xSemaphoreCreateMutex();
    // Before the first Wi-Fi call: the driver then keeps the network it is
    // given in RAM only, instead of writing its name and password to flash
    WiFi.persistent(false);
    // The core hands the name to the station whenever WiFi.mode() switches it
    // on, so it has to be there before the first time: set after, the first
    // join of each start asked for its address as "esp32s3-" and a number
    WiFi.setHostname(wifiHostName());
    Preferences prefs;
    prefs.begin("zvukopis", false);
    size_t len = prefs.isKey("networks") ? prefs.getBytesLength("networks") : 0;
    if (len % sizeof(WifiNetwork) == 0 && len <= sizeof(networks)) {
        networkTotal = prefs.getBytes("networks", networks, len) / sizeof(WifiNetwork);
    }
    forgetDriverCopy(prefs);
#ifdef HOTSPOT_PASSWORD
    strlcpy(hotspotPassword, HOTSPOT_PASSWORD, sizeof(hotspotPassword));
#else
    // Easy to type: no 0/o, 1/l/i. A stored one that is shorter (an earlier
    // release made 8 characters) is replaced.
    // The radio is off here, so esp_random() is not inside Espressif's "true
    // random" promise, but it is not a fixed sequence either: the bootloader
    // feeds the generator with noise from the ADC at every start
    // (bootloader_random_enable(), ESP-IDF 4.4.7 bootloader_esp32s3.c), and
    // on this chip an 8 MHz oscillator keeps stirring it afterwards.
    prefs.getString("hotspotpass", hotspotPassword, sizeof(hotspotPassword));
    if (strlen(hotspotPassword) < HOTSPOT_PASSWORD_LEN) {
        const char *alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
        for (size_t i = 0; i < HOTSPOT_PASSWORD_LEN; i++) hotspotPassword[i] = alphabet[esp_random() % strlen(alphabet)];
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

// wifiJoin() has told the driver to join one access point only, by its address
static volatile bool pinned = false;

// What the link does, on the log: a dropped connection and its reason, and
// the reconnection the driver makes on its own
static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.printf("[wifi] disconnected (reason %d)\n", info.wifi_sta_disconnected.reason);
        // The access point the join is pinned to is not there: gone between
        // the scan and the join, or switched off or out of reach later on.
        // The core keeps reconnecting with the settings the driver holds, and
        // would ask for that one address for good. So the address is taken
        // out of them: the next try looks for the network by its name, on
        // every channel, and takes the strongest access point it finds.
        // (Only the settings: with Wi-Fi off meanwhile, this does nothing.)
        if (pinned && info.wifi_sta_disconnected.reason == WIFI_REASON_NO_AP_FOUND) {
            wifi_config_t config;
            if (esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK) {
                config.sta.bssid_set = false;
                config.sta.channel = 0;
                config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
                if (esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK) {
                    pinned = false;
                    Serial.println("[wifi] that access point is gone: any other of the network will do");
                }
            }
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
        pinned = false; // Wi-Fi was switched off: there is no join to keep up
        break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[wifi] connected");
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[wifi] address %s\n", WiFi.localIP().toString().c_str());
        break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        Serial.println("[wifi] lost the address");
        break;
    default:
        break;
    }
}

WifiJoin wifiJoin(char *ssid, size_t len, bool (*abort)()) {
    if (!wifiKnowsAny()) return WIFI_NONE_KNOWN;
    static bool listening = false;
    if (!listening) {
        WiFi.onEvent(onWifiEvent);
        listening = true;
    }
    pinned = false;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false); // the radio stays awake while the web interface is up: it answers at once, and the link holds
    int found = WiFi.scanNetworks();
    WifiNetwork best = {};
    int bestRssi = -1000, bestChannel = 0;
    uint8_t bestBssid[6] = {};
    for (int i = 0; i < found; i++) {
        WifiNetwork known;
        if (WiFi.RSSI(i) > bestRssi && knownNetwork(WiFi.SSID(i), known)) {
            best = known;
            bestRssi = WiFi.RSSI(i);
            bestChannel = WiFi.channel(i);
            memcpy(bestBssid, WiFi.BSSID(i), sizeof(bestBssid));
        }
    }
    WiFi.scanDelete();
    if (bestRssi == -1000) {
        Serial.printf("[wifi] none of the known networks in reach (%d seen)\n", found);
        WiFi.mode(WIFI_OFF);
        return WIFI_NOT_IN_REACH;
    }

    // That very access point: a network may have several under one name (a
    // mesh, a repeater), and left to itself the driver joins the first one
    // its own scan meets, from channel 1 up, however weak. (Should it have
    // gone meanwhile, onWifiEvent() lets go of the address.)
    Serial.printf("[wifi] joining %s (%d dBm, channel %d)\n", best.ssid, bestRssi, bestChannel);
    pinned = true;
    WiFi.begin(best.ssid, best.password, bestChannel, bestBssid);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((abort && abort()) || millis() - start > CONNECT_TIMEOUT_MS) {
            Serial.printf("[wifi] couldn't join %s (status %d)\n", best.ssid, WiFi.status());
            pinned = false;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            return WIFI_JOIN_FAILED;
        }
        delay(100);
    }
    strlcpy(ssid, best.ssid, len);
    return WIFI_JOINED;
}

const char *wifiHostName() {
    static char name[14]; // "zvukopis-" and four digits
    if (!name[0]) {
        // Made once (wifiInit() asks first, before any other task runs), from
        // the last two bytes of the address the factory gave the chip
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        snprintf(name, sizeof(name), "zvukopis-%02x%02x", mac[4], mac[5]);
    }
    return name;
}

const char *wifiHotspotName() { return wifiHostName(); }
const char *wifiHotspotPassword() { return hotspotPassword; }
