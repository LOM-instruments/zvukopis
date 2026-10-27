#include "transfer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "board_power.h"
#include "clock.h"
#include "storage.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef HOTSPOT_PASSWORD
#define HOTSPOT_PASSWORD "zvukopis"
#endif

static const char *HOTSPOT_NAME = "zvukopis";
static const char *HOST_NAME = "zvukopis"; // http://zvukopis.local/
static const uint32_t JOIN_TIMEOUT_MS = 15000;
static const uint32_t IDLE_TIMEOUT_MS = 10 * 60 * 1000;
static const size_t SEND_CHUNK = 4096;

static WebServer server(80);
static TransferInfo info = {};
static volatile bool stopRequested = false;
static TaskHandle_t task = nullptr;
static uint32_t lastRequestAt = 0;

static const char *TAG_NAMES[TAG_COUNT] = {"", "human", "nature", "funny", "art", "other"};

static bool findMoment(int number, int &index, Moment &moment) {
    for (int i = 0; i < momentCount(); i++) {
        if (momentGet(i, moment) && moment.number == number) {
            index = i;
            return true;
        }
    }
    return false;
}

// Name for downloads: the card's file name, or the same pattern for moments in memory
static void downloadName(const Moment &moment, char *buf, size_t len) {
    if (moment.fileName[0]) {
        strlcpy(buf, moment.fileName, len);
    } else if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        strftime(buf, len, "%Y-%m-%d_%H-%M-%S.wav", &local);
    } else {
        snprintf(buf, len, "MOMENT_%04d.wav", moment.number);
    }
}

static void noteRequest() {
    lastRequestAt = millis();
    info.requests++;
}

static const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>zvukopis</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;max-width:40rem;margin:0 auto;padding:1rem;color:#111;background:#fff}
h1{margin:.2rem 0}.sub{color:#555;margin-bottom:1rem}
table{width:100%;border-collapse:collapse}td{padding:.55rem .3rem;border-top:1px solid #ddd;vertical-align:middle}
td.r{text-align:right;white-space:nowrap}a,button{font:inherit}
button{border:1px solid #111;background:#fff;border-radius:.4rem;padding:.3rem .6rem;cursor:pointer}
.tag{font-size:.85rem;border:1px solid #888;border-radius:1rem;padding:0 .45rem;color:#333}
.bar{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin-bottom:1rem}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}td{border-color:#333}
button{background:#111;color:#eee;border-color:#eee}.sub,.tag{color:#aaa}a{color:#9cf}}
</style></head><body><h1>zvukopis</h1>
)";

static const char PAGE_TAIL[] PROGMEM = R"(</table>
<script>
async function post(u){const r=await fetch(u,{method:'POST'});if(!r.ok)alert(await r.text());location.reload()}
function setClock(){post('/time?ms='+Date.now())}
function del(n){if(confirm('Delete Moment '+n+'? This can\'t be undone.'))post('/delete?n='+n)}
</script></body></html>)";

static void handleIndex() {
    noteRequest();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent_P(PAGE_HEAD);

    char buf[320];
    time_t now = time(nullptr);
    char clock[32] = "not set";
    if (clockValid()) {
        tm local;
        localtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%e %b %Y, %H:%M", &local);
    }
    int count = momentCount();
    int battery = batteryPercent();
    char batteryText[24] = "";
    if (battery >= 0) snprintf(batteryText, sizeof(batteryText), " &middot; battery %d%%", battery);
    snprintf(buf, sizeof(buf), "<div class=sub>%d moment%s &middot; device clock %s%s</div>"
                               "<div class=bar><button onclick=setClock()>Set the device clock to this device's time</button></div><table>",
             count, count == 1 ? "" : "s", clock, batteryText);
    server.sendContent(buf);

    for (int i = 0; i < count; i++) {
        Moment moment;
        if (!momentGet(i, moment)) break;
        char when[32] = "", name[40];
        if (moment.recordedAt) {
            tm local;
            localtime_r(&moment.recordedAt, &local);
            strftime(when, sizeof(when), "%e %b %Y %H:%M:%S", &local);
        }
        downloadName(moment, name, sizeof(name));
        int seconds = (int)((moment.length + SAMPLE_RATE / 2) / SAMPLE_RATE);
        char tag[48] = "";
        if (moment.tag != TAG_NONE) snprintf(tag, sizeof(tag), " <span class=tag>%s</span>", TAG_NAMES[moment.tag]);
        snprintf(buf, sizeof(buf),
                 "<tr><td><b>Moment %d</b>%s<br><small>%s</small></td><td class=r>%d:%02d</td>"
                 "<td class=r><a href=\"/moment?n=%d\" download=\"%s\">Download</a></td>"
                 "<td class=r><button onclick=del(%d)>Delete</button></td></tr>",
                 moment.number, tag, when, seconds / 60, seconds % 60, moment.number, name, moment.number);
        server.sendContent(buf);
    }
    if (count == 0) server.sendContent("<tr><td>No moments yet</td></tr>");
    server.sendContent_P(PAGE_TAIL);
    server.sendContent("");
}

static void handleMoment() {
    noteRequest();
    int index;
    Moment moment;
    if (!findMoment(server.arg("n").toInt(), index, moment)) {
        server.send(404, "text/plain", "No such moment");
        return;
    }
    char name[40], disposition[64];
    downloadName(moment, name, sizeof(name));
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    server.sendHeader("Content-Disposition", disposition);

    if (!moment.samples) {
        File f = storageOpenMoment(moment);
        if (!f) {
            server.send(500, "text/plain", "Can't open the file on the SD card");
            return;
        }
        f.seek(0);
        server.streamFile(f, "audio/wav");
        f.close();
        return;
    }

    uint8_t header[WAV_HEADER_BYTES];
    size_t headerSize = storageWavHeader(moment, header);
    size_t dataSize = moment.length * sizeof(int16_t);
    server.setContentLength(headerSize + dataSize);
    server.send(200, "audio/wav", "");
    WiFiClient client = server.client();
    client.write(header, headerSize);
    // Its samples stay put unless the moment is deleted, which only this server
    // can do while the transfer screen is up (from another browser tab, say)
    const uint8_t *data = (const uint8_t *)moment.samples;
    for (size_t sent = 0; sent < dataSize && client.connected();) {
        size_t n = client.write(data + sent, min(SEND_CHUNK, dataSize - sent));
        if (n == 0) break;
        sent += n;
        lastRequestAt = millis();
    }
}

static void handleTime() {
    noteRequest();
    int64_t ms = atoll(server.arg("ms").c_str());
    if (ms < 1700000000000LL) {
        server.send(400, "text/plain", "Bad time");
        return;
    }
    clockSet((time_t)(ms / 1000));
    server.send(200, "text/plain", "ok");
}

static void handleDelete() {
    noteRequest();
    int index;
    Moment moment;
    if (!findMoment(server.arg("n").toInt(), index, moment)) {
        server.send(404, "text/plain", "No such moment");
        return;
    }
    audioDelete(index);
    delay(300); // let the audio task carry it out before the page reloads
    server.send(200, "text/plain", "ok");
}

static bool joinNetwork() {
#ifdef WIFI_SSID
    Serial.printf("[transfer] joining %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOST_NAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (stopRequested || millis() - start > JOIN_TIMEOUT_MS) {
            Serial.printf("[transfer] couldn't join (status %d)\n", WiFi.status());
            WiFi.disconnect(true);
            return false;
        }
        delay(100);
    }
    strlcpy(info.network, WIFI_SSID, sizeof(info.network));
    info.password[0] = 0;
    strlcpy(info.address, WiFi.localIP().toString().c_str(), sizeof(info.address));
    info.hotspot = false;
    return true;
#else
    return false;
#endif
}

static void openHotspot() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(HOTSPOT_NAME, HOTSPOT_PASSWORD);
    strlcpy(info.network, HOTSPOT_NAME, sizeof(info.network));
    strlcpy(info.password, HOTSPOT_PASSWORD, sizeof(info.password));
    strlcpy(info.address, WiFi.softAPIP().toString().c_str(), sizeof(info.address));
    info.hotspot = true;
}

static void transferTask(void *) {
    bool joined = joinNetwork();
    if (!stopRequested) {
        if (joined) clockNtp(); // already online, so the clock gets corrected too
        else openHotspot();
        Serial.printf("[transfer] serving http://%s/ on %s\n", info.address, info.network);

        MDNS.begin(HOST_NAME);
        MDNS.addService("http", "tcp", 80);
        server.on("/", HTTP_GET, handleIndex);
        server.on("/moment", HTTP_GET, handleMoment);
        server.on("/time", HTTP_POST, handleTime);
        server.on("/delete", HTTP_POST, handleDelete);
        server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
        server.begin();
        lastRequestAt = millis();
        info.state = TRANSFER_READY;

        while (!stopRequested && millis() - lastRequestAt < IDLE_TIMEOUT_MS) {
            server.handleClient();
            delay(2);
        }
        server.stop();
        MDNS.end();
    }
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[transfer] stopped, Wi-Fi off");
    info.state = TRANSFER_OFF;
    task = nullptr;
    vTaskDelete(nullptr);
}

void transferStart() {
    if (task) return;
    stopRequested = false;
    info = {};
    info.state = TRANSFER_CONNECTING;
    xTaskCreatePinnedToCore(transferTask, "transfer", 8192, NULL, 1, &task, 0);
}

void transferStop() {
    if (!task) return;
    stopRequested = true;
    uint32_t start = millis();
    while (task && millis() - start < 5000) delay(20);
}

TransferInfo transferInfo() { return info; }
