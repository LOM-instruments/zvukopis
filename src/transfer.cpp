#include "transfer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "board_power.h"
#include "clock.h"
#include "config.h"
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

static volatile bool hotspotOnly = false;

// Previews of the category icons, in the order of CategoryIcon
static const char *const ICON_EMOJI[ICON_COUNT] = {"&#x1F464;", "&#x1F333;", "&#x1F642;", "&#x1F3A8;", "&#x2726;",
                                                   "&#x3030;",  "&#x1F3B5;", "&#x2665;",  "&#x1F4A7;", "&#x2600;"};

// For text people typed (category names) going into the page
static void htmlEscape(const char *in, char *out, size_t len) {
    size_t n = 0;
    for (; *in && n + 7 < len; in++) {
        switch (*in) {
        case '<': n += snprintf(out + n, len - n, "&lt;"); break;
        case '>': n += snprintf(out + n, len - n, "&gt;"); break;
        case '&': n += snprintf(out + n, len - n, "&amp;"); break;
        case '"': n += snprintf(out + n, len - n, "&quot;"); break;
        default: out[n++] = *in; break;
        }
    }
    out[n] = 0;
}

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
body{font-family:"Atkinson Hyperlegible Next","Atkinson Hyperlegible",-apple-system,system-ui,sans-serif;
max-width:40rem;margin:0 auto;padding:1rem;color:#111;background:#fff;line-height:1.4}
h1{margin:.2rem 0}h2{margin:2rem 0 .4rem;font-size:1.15rem}.sub,small,.note{color:#555}.note{font-size:.9rem}
table{width:100%;border-collapse:collapse}td{padding:.55rem .3rem;border-top:1px solid #ddd;vertical-align:middle}
td.r{text-align:right;white-space:nowrap}a,button,input,select{font:inherit}
button{border:1px solid #111;background:#fff;color:#111;border-radius:.4rem;padding:.3rem .7rem;cursor:pointer}
input,select{border:1px solid #888;border-radius:.4rem;padding:.25rem .4rem;background:inherit;color:inherit}
.tag{font-size:.85rem;border:1px solid #888;border-radius:1rem;padding:0 .45rem;white-space:nowrap}
.row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:.5rem 0}
input[type=number]{width:4.5rem}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}td{border-color:#333}
button{background:#111;color:#eee;border-color:#eee}.sub,small,.note{color:#aaa}a{color:#9cf}}
</style></head><body><h1>zvukopis</h1>
)";

static const char PAGE_TAIL[] PROGMEM = R"(
<script>
async function post(u){const r=await fetch(u,{method:'POST'});if(!r.ok)alert(await r.text());location.reload()}
function setClock(){post('/time?ms='+Date.now())}
function del(n){if(confirm('Delete Moment '+n+'? This can\'t be undone.'))post('/delete?n='+n)}
function addSchedule(){
 const w=document.getElementById('when').value;if(!w){alert('Choose a date and time');return}
 const s=Math.round(new Date(w).getTime()/1000);
 const d=(+document.getElementById('min').value||0)*60+(+document.getElementById('sec').value||0);
 if(d<1){alert('Choose how long to record');return}
 post('/schedule/add?start='+s+'&seconds='+d+'&daily='+(document.getElementById('daily').checked?1:0))}
function unschedule(s){post('/schedule/remove?start='+s)}
function saveCategories(){
 const q=new URLSearchParams();
 for(let i=1;i<=5;i++){q.set('name'+i,document.getElementById('name'+i).value);q.set('icon'+i,document.getElementById('icon'+i).value)}
 post('/categories?'+q)}
</script></body></html>)";

static void sendMoments(char *buf, size_t len) {
    int count = momentCount();
    server.sendContent("<h2>Moments</h2><table>");
    for (int i = 0; i < count; i++) {
        Moment moment;
        if (!momentGet(i, moment)) break;
        char when[32] = "", name[40], tag[64] = "";
        if (moment.recordedAt) {
            tm local;
            localtime_r(&moment.recordedAt, &local);
            strftime(when, sizeof(when), "%e %b %Y %H:%M:%S", &local);
        }
        downloadName(moment, name, sizeof(name));
        int seconds = (int)((moment.length + SAMPLE_RATE / 2) / SAMPLE_RATE);
        if (moment.tag != TAG_NONE) {
            Category c = configCategory(moment.tag);
            char escaped[CATEGORY_NAME_LEN * 6];
            htmlEscape(c.name, escaped, sizeof(escaped));
            snprintf(tag, sizeof(tag), " <span class=tag>%s %s</span>", ICON_EMOJI[c.icon], escaped);
        }
        snprintf(buf, len,
                 "<tr><td><b>Moment %d</b>%s<br><small>%s</small></td><td class=r>%d:%02d</td>"
                 "<td class=r><a href=\"/moment?n=%d\" download=\"%s\">Download</a></td>"
                 "<td class=r><button onclick=del(%d)>Delete</button></td></tr>",
                 moment.number, tag, when, seconds / 60, seconds % 60, moment.number, name, moment.number);
        server.sendContent(buf);
    }
    if (count == 0) server.sendContent("<tr><td>No moments yet</td></tr>");
    server.sendContent("</table>");
}

static void sendSchedule(char *buf, size_t len) {
    server.sendContent("<h2>Scheduled recordings</h2>");
    if (!clockValid()) server.sendContent("<p class=note>The device clock isn't set, so nothing will record on time. Set it above first.</p>");
    server.sendContent("<table>");
    Schedule s;
    for (int i = 0; scheduleGet(i, s); i++) {
        time_t start = (time_t)s.start;
        tm local;
        localtime_r(&start, &local);
        char when[40];
        strftime(when, sizeof(when), s.daily ? "every day at %H:%M:%S<br><small>next %a %e %b</small>"
                                              : "%a %e %b %Y, %H:%M:%S", &local);
        snprintf(buf, len, "<tr><td>%s</td><td class=r>%lu:%02lu</td><td class=r><button onclick=unschedule(%lld)>Remove</button></td></tr>",
                 when, (unsigned long)(s.seconds / 60), (unsigned long)(s.seconds % 60), (long long)s.start);
        server.sendContent(buf);
    }
    if (scheduleCount() == 0) server.sendContent("<tr><td>None yet</td></tr>");
    int longest = (int)audioLongestRecording();
    snprintf(buf, len,
             "</table><div class=row><input type=datetime-local id=when step=1>"
             "<input type=number id=min min=0 value=1> min <input type=number id=sec min=0 max=59 value=0> s"
             "<label><input type=checkbox id=daily> every day</label><button onclick=addSchedule()>Add</button></div>"
             "<p class=note>Times are in this browser's time zone. zvukopis wakes for them, also from sleep "
             "(the screen stays as it is). A recording can last up to %d:%02d for now: it is held in memory before "
             "being saved. Up to %d entries.</p>",
             longest / 60, longest % 60, MAX_SCHEDULES);
    server.sendContent(buf);
}

static void sendCategories(char *buf, size_t len) {
    server.sendContent("<h2>Categories</h2><p class=note>What a moment can be tagged as, from its long-press menu on "
                       "zvukopis. Renaming one renames it for the moments already tagged with it.</p>");
    for (int slot = 1; slot <= CATEGORY_COUNT; slot++) {
        Category c = configCategory(slot);
        char escaped[CATEGORY_NAME_LEN * 6];
        htmlEscape(c.name, escaped, sizeof(escaped));
        snprintf(buf, len, "<div class=row><select id=icon%d>", slot);
        server.sendContent(buf);
        for (int icon = 0; icon < ICON_COUNT; icon++) {
            snprintf(buf, len, "<option value=%d%s>%s %s</option>", icon, icon == c.icon ? " selected" : "",
                     ICON_EMOJI[icon], ICON_NAMES[icon]);
            server.sendContent(buf);
        }
        snprintf(buf, len, "</select><input id=name%d maxlength=%d value=\"%s\"></div>", slot, CATEGORY_NAME_LEN - 1, escaped);
        server.sendContent(buf);
    }
    server.sendContent("<div class=row><button onclick=saveCategories()>Save categories</button></div>");
}

static void handleIndex() {
    noteRequest();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent_P(PAGE_HEAD);

    char buf[512];
    time_t now = time(nullptr);
    char clock[32] = "not set";
    if (clockValid()) {
        tm local;
        localtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%e %b %Y, %H:%M", &local);
    }
    int battery = batteryPercent();
    char batteryText[24] = "";
    if (battery >= 0) snprintf(batteryText, sizeof(batteryText), " &middot; battery %d%%", battery);
    snprintf(buf, sizeof(buf), "<div class=sub>Device clock %s%s</div>"
                               "<div class=row><button onclick=setClock()>Set the device clock to this device's time</button></div>",
             clock, batteryText);
    server.sendContent(buf);
    sendMoments(buf, sizeof(buf));
    sendSchedule(buf, sizeof(buf));
    sendCategories(buf, sizeof(buf));
    server.sendContent_P(PAGE_TAIL);
    server.sendContent("");
}

static void handleScheduleAdd() {
    noteRequest();
    Schedule s = {};
    s.start = atoll(server.arg("start").c_str());
    s.seconds = server.arg("seconds").toInt();
    s.daily = server.arg("daily") == "1";
    if (s.seconds < 1 || s.seconds > 24 * 3600 || s.start < 1700000000) {
        server.send(400, "text/plain", "Choose a time and a length between 1 s and 24 h");
        return;
    }
    if (!s.daily && s.start + s.seconds <= time(nullptr)) {
        server.send(400, "text/plain", "That time has already passed (by the device clock)");
        return;
    }
    if (!scheduleAdd(s)) {
        server.send(400, "text/plain", "The schedule is full");
        return;
    }
    server.send(200, "text/plain", "ok");
}

static void handleScheduleRemove() {
    noteRequest();
    int64_t start = atoll(server.arg("start").c_str());
    Schedule s;
    for (int i = 0; scheduleGet(i, s); i++) {
        if (s.start == start) {
            scheduleRemove(i);
            break;
        }
    }
    server.send(200, "text/plain", "ok");
}

static void handleCategories() {
    noteRequest();
    for (int slot = 1; slot <= CATEGORY_COUNT; slot++) {
        Category c = {};
        String name = server.arg("name" + String(slot));
        name.trim();
        // The screen's fonts have only ASCII
        int n = 0;
        for (unsigned i = 0; i < name.length() && n < CATEGORY_NAME_LEN - 1; i++) {
            if (name[i] >= 0x20 && name[i] < 0x7F) c.name[n++] = name[i];
        }
        if (n == 0) c = configCategory(slot); // left empty: keep the name
        c.icon = server.arg("icon" + String(slot)).toInt();
        configSetCategory(slot, c);
    }
    server.send(200, "text/plain", "ok");
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
    bool joined = !hotspotOnly && joinNetwork();
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
        server.on("/schedule/add", HTTP_POST, handleScheduleAdd);
        server.on("/schedule/remove", HTTP_POST, handleScheduleRemove);
        server.on("/categories", HTTP_POST, handleCategories);
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

void transferStart(bool hotspot) {
    if (task) return;
    hotspotOnly = hotspot;
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
