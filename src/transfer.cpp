#include "transfer.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "board_power.h"
#include "clock.h"
#include "config.h"
#include "storage.h"
#include "networks.h"
#include "ui.h"

static const char *HOST_NAME = "zvukopis"; // http://zvukopis.local/
static const uint32_t IDLE_TIMEOUT_MS = 10 * 60 * 1000;
static const size_t SEND_CHUNK = 4096;

static WebServer server(80);
static DNSServer dns; // on the hotspot every name leads here, so phones open the page by themselves
static TransferInfo info = {};
static volatile bool stopRequested = false;
static TaskHandle_t task = nullptr;
static uint32_t lastRequestAt = 0;

static volatile bool hotspotOnly = false;

// The category icons as SVG, drawn like on zvukopis (ui.cpp, iconCategory),
// in the order of CategoryIcon; they take the text colour
static const char *const ICON_SVG[ICON_COUNT] = {
    "<circle cy=-4 r=4 /><rect x=-8 y=1 width=16 height=9 rx=4 />",
    "<path d='M0-10L-8 4H8Z'/><rect x=-1.7 y=4 width=3.4 height=6 />",
    "<circle r=9 fill=none stroke=currentColor stroke-width=2 /><circle cx=-3.5 cy=-3 r=1.4 /><circle cx=3.5 cy=-3 r=1.4 />"
    "<path d='M5 2.3A5.5 5.5 0 0 1-5 2.3' fill=none stroke=currentColor stroke-width=1.7 />",
    "<path fill-rule=evenodd d='M-10 0a10 10 0 1 0 20 0a10 10 0 1 0-20 0ZM1.5 4a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0Z"
    "M-6.2-1.5a1.7 1.7 0 1 0 3.4 0a1.7 1.7 0 1 0-3.4 0ZM-2.7-5a1.7 1.7 0 1 0 3.4 0a1.7 1.7 0 1 0-3.4 0Z"
    "M1.8-3.5a1.7 1.7 0 1 0 3.4 0a1.7 1.7 0 1 0-3.4 0Z'/>",
    "<path d='M0-10L3.3-3.3L10 0L3.3 3.3L0 10L-3.3 3.3L-10 0L-3.3-3.3Z'/>",
    "<path d='M-10 0Q-7.5-9-5 0T0 0T5 0T10 0' fill=none stroke=currentColor stroke-width=2.6 />",
    "<circle cx=-3 cy=5.5 r=3.8 /><rect x=-0.2 y=-10 width=2 height=15.5 /><path d='M1.8-10V-4L7-3Z'/>",
    "<circle cx=-4.5 cy=-2.5 r=4.8 /><circle cx=4.5 cy=-2.5 r=4.8 /><path d='M-9.2-1H9.2L0 10Z'/>",
    "<circle cy=3 r=6 /><path d='M0-10L-5.7 1.5H5.7Z'/>",
    "<circle r=4.5 /><path d='M6.5 0H10M-6.5 0H-10M0 6.5V10M0-6.5V-10M4.6 4.6L7.1 7.1M-4.6-4.6L-7.1-7.1M4.6-4.6L7.1-7.1"
    "M-4.6 4.6L-7.1 7.1' stroke=currentColor stroke-width=1.6 />",
};

static String iconSvg(int icon, int size = 20) {
    return String("<svg class=icon width=") + size + " height=" + size + " viewBox='-11 -11 22 22' fill=currentColor>" +
           ICON_SVG[icon < ICON_COUNT ? icon : ICON_STAR] + "</svg>";
}

// Common time zones as POSIX rules, with the IANA names browsers report
struct Zone {
    const char *label;
    const char *posix;
    const char *iana; // space separated
};
static const Zone ZONES[] = {
    {"UTC", "UTC0", "UTC Etc/UTC"},
    {"London, Dublin, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0", "Europe/London Europe/Dublin Europe/Lisbon Atlantic/Canary"},
    {"Central Europe (Berlin, Paris, Prague, Bratislava, Rome, Madrid)", "CET-1CEST,M3.5.0,M10.5.0/3",
     "Europe/Berlin Europe/Paris Europe/Prague Europe/Bratislava Europe/Vienna Europe/Rome Europe/Madrid "
     "Europe/Warsaw Europe/Amsterdam Europe/Brussels Europe/Budapest Europe/Copenhagen Europe/Oslo "
     "Europe/Stockholm Europe/Zurich Europe/Belgrade Europe/Zagreb Europe/Ljubljana"},
    {"Eastern Europe (Athens, Helsinki, Kyiv, Bucharest)", "EET-2EEST,M3.5.0/3,M10.5.0/4",
     "Europe/Athens Europe/Helsinki Europe/Kiev Europe/Kyiv Europe/Bucharest Europe/Sofia Europe/Riga "
     "Europe/Tallinn Europe/Vilnius"},
    {"Istanbul", "<+03>-3", "Europe/Istanbul"},
    {"Moscow", "MSK-3", "Europe/Moscow"},
    {"Lagos", "WAT-1", "Africa/Lagos"},
    {"Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", "Africa/Cairo"},
    {"Johannesburg", "SAST-2", "Africa/Johannesburg"},
    {"Nairobi", "EAT-3", "Africa/Nairobi"},
    {"Tehran", "<+0330>-3:30", "Asia/Tehran"},
    {"Dubai", "<+04>-4", "Asia/Dubai"},
    {"Karachi", "PKT-5", "Asia/Karachi"},
    {"India", "IST-5:30", "Asia/Kolkata Asia/Calcutta"},
    {"Kathmandu", "<+0545>-5:45", "Asia/Kathmandu"},
    {"Dhaka", "<+06>-6", "Asia/Dhaka"},
    {"Bangkok, Jakarta, Hanoi", "<+07>-7", "Asia/Bangkok Asia/Jakarta Asia/Ho_Chi_Minh"},
    {"China, Singapore, Perth", "CST-8",
     "Asia/Shanghai Asia/Hong_Kong Asia/Singapore Asia/Taipei Asia/Kuala_Lumpur Asia/Manila Australia/Perth"},
    {"Tokyo, Seoul", "JST-9", "Asia/Tokyo Asia/Seoul"},
    {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", "Australia/Adelaide"},
    {"Brisbane", "AEST-10", "Australia/Brisbane"},
    {"Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney Australia/Melbourne Australia/Hobart"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", "Pacific/Auckland"},
    {"Honolulu", "HST10", "Pacific/Honolulu"},
    {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", "America/Anchorage"},
    {"Los Angeles, Vancouver", "PST8PDT,M3.2.0,M11.1.0", "America/Los_Angeles America/Vancouver America/Tijuana"},
    {"Denver", "MST7MDT,M3.2.0,M11.1.0", "America/Denver America/Edmonton"},
    {"Phoenix", "MST7", "America/Phoenix"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0", "America/Chicago America/Winnipeg"},
    {"Mexico City", "CST6", "America/Mexico_City"},
    {"New York, Toronto", "EST5EDT,M3.2.0,M11.1.0", "America/New_York America/Toronto America/Detroit"},
    {"Bogot\u00e1, Lima", "<-05>5", "America/Bogota America/Lima"},
    {"Halifax", "AST4ADT,M3.2.0,M11.1.0", "America/Halifax"},
    {"S\u00e3o Paulo, Buenos Aires", "<-03>3", "America/Sao_Paulo America/Argentina/Buenos_Aires America/Montevideo"},
};

// For text people typed (category names, network names) going into the page
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

// For network names in the scan results
static String jsonString(const String &in) {
    String out = "\"";
    for (unsigned i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') out += '\\';
        if ((uint8_t)c < 0x20) continue;
        out += c;
    }
    return out + "\"";
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
h1{margin:.2rem 0}h2{margin:1.6rem 0 .4rem;font-size:1.15rem}.sub,small,.note{color:#555}.note{font-size:.9rem}
.tabs{display:flex;gap:.3rem;margin:1rem 0 0;border-bottom:1px solid #ddd}
.tabs button{border:none;border-bottom:3px solid transparent;border-radius:0;padding:.5rem 1rem;background:none;color:inherit;font-weight:600;opacity:.55}
.tabs button.on{border-bottom-color:currentColor;opacity:1}.tab{display:none}.tab.on{display:block}
#recstatus{font-size:1.2rem}
table{width:100%;border-collapse:collapse}td{padding:.55rem .3rem;border-top:1px solid #ddd;vertical-align:middle}
td.r{text-align:right;white-space:nowrap}a,button,input,select{font:inherit}
.icon{vertical-align:-4px}.pick{display:inline-flex;gap:2px;flex-wrap:wrap}
.pick label{cursor:pointer;padding:5px;border-radius:.4rem;border:1px solid transparent;line-height:0}
.pick input{display:none}.pick input:checked+svg{color:#fff;background:#111;border-radius:.3rem;box-shadow:0 0 0 4px #111}
button{border:1px solid #111;background:#fff;color:#111;border-radius:.4rem;padding:.3rem .7rem;cursor:pointer}
input,select{border:1px solid #888;border-radius:.4rem;padding:.25rem .4rem;background:inherit;color:inherit;max-width:100%}
.tag{font-size:.85rem;border:1px solid #888;border-radius:1rem;padding:0 .45rem;white-space:nowrap}
.row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:.5rem 0}
input[type=number]{width:4.5rem}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}td{border-color:#333}.tabs{border-color:#333}
.pick input:checked+svg{color:#111;background:#eee;box-shadow:0 0 0 4px #eee}
button{background:#111;color:#eee;border-color:#eee}.sub,small,.note{color:#aaa}a{color:#9cf}}
</style></head><body><h1>zvukopis</h1>
)";

static const char PAGE_TAIL[] PROGMEM = R"(
<script>
const $=id=>document.getElementById(id);
async function post(u,body){const r=await fetch(u,{method:'POST',body:body});if(!r.ok)alert(await r.text());location.reload()}
function setClock(){post('/time?ms='+Date.now())}
function showTab(t){for(const n of ['live','settings']){$(n).classList.toggle('on',n===t);$('tab-'+n).classList.toggle('on',n===t)}
 if(location.hash!=='#'+t)history.replaceState(null,'','#'+t)}
showTab(location.hash==='#settings'||location.hash==='#live'?location.hash.slice(1):firstTab);
function del(n){if(confirm('Delete Moment '+n+'? This can\'t be undone.'))post('/delete?n='+n)}
function play(n){const p=$('player');p.src='/moment?n='+n+'&play=1';p.play()}
async function setGain(){const r=await fetch('/gain?db='+$('gain').value,{method:'POST'});if(!r.ok)alert(await r.text());poll()}
function addSchedule(){
 const d=$('date').value,t=$('time').value.trim();
 if(!d){alert('Choose a date');return}
 if(!/^([01]?[0-9]|2[0-3]):[0-5][0-9](:[0-5][0-9])?$/.test(t)){alert('Write the time as 24-hour HH:MM or HH:MM:SS, for example 06:30');return}
 const s=(+$('min').value||0)*60+(+$('sec').value||0);
 if(s<1){alert('Choose how long to record');return}
 post('/schedule/add?date='+d+'&time='+t+'&seconds='+s+'&daily='+($('daily').checked?1:0))}
function unschedule(s){post('/schedule/remove?start='+s)}
function saveCategories(){
 const q=new URLSearchParams();
 for(let i=1;i<=5;i++){q.set('name'+i,$('name'+i).value);q.set('icon'+i,document.querySelector('input[name=icon'+i+']:checked').value)}
 post('/categories?'+q)}
function zoneChanged(){$('customtz').style.display=$('tz').value==='custom'?'':'none'}
function useBrowserZone(){
 const z=Intl.DateTimeFormat().resolvedOptions().timeZone;
 for(const o of $('tz').options)if((o.dataset.iana||'').split(' ').includes(z)){$('tz').value=o.value;zoneChanged();return}
 alert('zvukopis has no rule for '+z+'. Choose the nearest zone, or enter a POSIX rule under Custom.')}
function saveZone(){const v=$('tz').value==='custom'?$('customtz').value.trim():$('tz').value;
 if(!v){alert('Enter a POSIX time zone rule');return} post('/timezone',new URLSearchParams({tz:v}))}
async function scan(){
 $('ssid').innerHTML='<option>Looking for networks...</option>';
 try{const nets=await(await fetch('/wifi/scan')).json();
  $('ssid').innerHTML=nets.map(n=>'<option>'+n.ssid.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</option>').join('')+'<option value="">Other...</option>';
 }catch(e){$('ssid').innerHTML='<option value="">Other...</option>'}
 ssidChanged()}
function ssidChanged(){$('otherssid').style.display=$('ssid').value===''?'':'none'}
function saveWifi(){const s=$('ssid').value||$('otherssid').value.trim();
 if(!s){alert('Choose or enter a network');return}
 post('/wifi/save',new URLSearchParams({ssid:s,password:$('pass').value}))}
function forget(i,n){if(confirm('Forget '+n+'?'))post('/wifi/forget?i='+i)}
async function record(start){
 const s=(+$('recmin').value||0)*60+(+$('recsec').value||0);
 const r=await fetch(start?'/record/start?seconds='+s:'/record/stop',{method:'POST'});
 if(!r.ok)alert(await r.text());poll()}
let moments=null;
async function poll(){
 try{const st=await(await fetch('/status')).json();
  const t=s=>Math.floor(s/60)+':'+String(Math.floor(s%60)).padStart(2,'0');
  $('recstatus').textContent=st.state=='recording'?'Recording '+t(st.seconds)+' of up to '+t(st.limit)
   :st.state=='saving'?'Saving...':st.state=='playing'?'Playing on zvukopis':'Ready to record';
  $('recstart').disabled=st.state=='recording'||st.state=='saving';$('recstop').disabled=st.state!='recording';
  $('gainnow').textContent=st.gainSetting<0?'now '+st.gain+' dB':'';
  if(moments!==null&&st.moments!=moments)location.reload();moments=st.moments;
 }catch(e){}}
setInterval(poll,1000);poll();
if($('ssid'))scan();
</script></body></html>)";

static const int GAIN_STEPS[] = {GAIN_AUTO, 0, 6, 12, 18, 24, 30, 36, 42, 48, 54};

static void sendRecord(char *buf, size_t len) {
    int longest = (int)audioLongestRecording();
    snprintf(buf, len, "<h2>Record</h2><div class=row><b id=recstatus>&nbsp;</b></div>"
                       "<div class=row><button id=recstart onclick=record(1)>Start recording</button>"
                       "<button id=recstop onclick=record(0) disabled>Stop</button>"
                       "<span>for <input type=number id=recmin min=0 value=0> min <input type=number id=recsec min=0 max=59 value=0> s"
                       "</span></div><p class=note>Leave the length at 0 to record until you press Stop (up to %d:%02d). "
                       "Wi-Fi stays on meanwhile, which may add a little noise.</p>",
             longest / 60, longest % 60);
    server.sendContent(buf);

    int setting = audioMicGainSetting();
    server.sendContent("<div class=row>Mic gain <select id=gain onchange=setGain()>");
    for (int db : GAIN_STEPS) {
        if (db == GAIN_AUTO) snprintf(buf, len, "<option value=%d%s>Auto</option>", db, setting == db ? " selected" : "");
        else snprintf(buf, len, "<option value=%d%s>%d dB</option>", db, setting == db ? " selected" : "", db);
        server.sendContent(buf);
    }
    server.sendContent("</select> <span id=gainnow class=sub></span></div>");
}

static void sendMoments(char *buf, size_t len) {
    int count = momentCount();
    server.sendContent("<h2>Moments</h2><audio id=player controls style=\"width:100%;margin:.3rem 0\"></audio><table>");
    for (int i = 0; i < count; i++) {
        Moment moment;
        if (!momentGet(i, moment)) break;
        char when[32] = "", name[40], tag[1024] = "";
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
            snprintf(tag, sizeof(tag), " <span class=tag>%s %s</span>", iconSvg(c.icon, 14).c_str(), escaped);
        }
        snprintf(buf, len,
                 "<tr><td><b>Moment %d</b>%s<br><small>%s</small></td><td class=r>%d:%02d</td>"
                 "<td class=r><button onclick=play(%d)>Play</button></td>"
                 "<td class=r><a href=\"/moment?n=%d\" download=\"%s\">Download</a></td>"
                 "<td class=r><button onclick=del(%d)>Delete</button></td></tr>",
                 moment.number, tag, when, seconds / 60, seconds % 60, moment.number, moment.number, name, moment.number);
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
    // The form starts at the next whole minute, in zvukopis's local time
    char today[12] = "", nextMinute[10] = "";
    if (clockValid()) {
        time_t t = time(nullptr) + 60;
        tm local;
        localtime_r(&t, &local);
        strftime(today, sizeof(today), "%Y-%m-%d", &local);
        strftime(nextMinute, sizeof(nextMinute), "%H:%M:00", &local);
    }
    snprintf(buf, len,
             "</table><div class=row><input type=date id=date value=\"%s\">"
             "<input id=time size=8 placeholder=HH:MM:SS value=\"%s\" inputmode=numeric> (24 h)</div>"
             "<div class=row><input type=number id=min min=0 value=1> min <input type=number id=sec min=0 max=59 value=0> s"
             "<label><input type=checkbox id=daily> every day</label><button onclick=addSchedule()>Add</button></div>"
             "<p class=note>Times are zvukopis's local time (see Time zone below). It wakes for them, also from "
             "sleep (the screen stays as it is). A recording can last up to %d:%02d for now: it is held in memory "
             "before being saved. Up to %d entries.</p>",
             today, nextMinute, longest / 60, longest % 60, MAX_SCHEDULES);
    server.sendContent(buf);
}

static void sendCategories(char *buf, size_t len) {
    server.sendContent("<h2>Categories</h2><p class=note>What a moment can be tagged as, from its long-press menu on "
                       "zvukopis. Renaming one renames it for the moments already tagged with it.</p>");
    for (int slot = 1; slot <= CATEGORY_COUNT; slot++) {
        Category c = configCategory(slot);
        char escaped[CATEGORY_NAME_LEN * 6];
        htmlEscape(c.name, escaped, sizeof(escaped));
        snprintf(buf, len, "<div class=row><input id=name%d maxlength=%d value=\"%s\"><span class=pick>",
                 slot, CATEGORY_NAME_LEN - 1, escaped);
        server.sendContent(buf);
        for (int icon = 0; icon < ICON_COUNT; icon++) {
            snprintf(buf, len, "<label title=%s><input type=radio name=icon%d value=%d%s>", ICON_NAMES[icon], slot, icon,
                     icon == c.icon ? " checked" : "");
            server.sendContent(buf);
            server.sendContent(iconSvg(icon) + "</label>");
        }
        server.sendContent("</span></div>");
    }
    server.sendContent("<div class=row><button onclick=saveCategories()>Save categories</button></div>");
}

static void sendTimeZone(char *buf, size_t len) {
    const char *current = clockTimeZone();
    bool known = false;
    server.sendContent("<h2>Time</h2><div class=row><button onclick=setClock()>Set the device clock to this device's time</button></div>"
                       "<div class=row><select id=tz onchange=zoneChanged()>");
    for (const Zone &z : ZONES) {
        bool chosen = strcmp(z.posix, current) == 0 && !known;
        known |= chosen;
        snprintf(buf, len, "<option value=\"%s\" data-iana=\"%s\"%s>%s</option>", z.posix, z.iana,
                 chosen ? " selected" : "", z.label);
        server.sendContent(buf);
    }
    char escaped[160];
    htmlEscape(current, escaped, sizeof(escaped));
    snprintf(buf, len, "<option value=custom%s>Custom POSIX rule</option></select>"
                       "<input id=customtz size=28 value=\"%s\" style=\"%s\"></div>"
                       "<div class=row><button onclick=useBrowserZone()>Use this browser's zone</button>"
                       "<button onclick=saveZone()>Save time zone</button></div>"
                       "<p class=note>Used for the clock, file names, the calendar and scheduled recordings. "
                       "Daylight saving time follows by itself.</p>",
             known ? "" : " selected", escaped, known ? "display:none" : "");
    server.sendContent(buf);
}

static void sendWifi(char *buf, size_t len) {
    bool firstTime = info.hotspot && wifiNetworkCount() == 0;
    server.sendContent(firstTime ? "<h2>Connect zvukopis to your Wi-Fi</h2>" : "<h2>Wi-Fi networks</h2>");
    server.sendContent("<table>");
    WifiNetwork n;
    for (int i = 0; wifiNetworkGet(i, n); i++) {
        char escaped[200];
        htmlEscape(n.ssid, escaped, sizeof(escaped));
        String js = jsonString(n.ssid);
        js.replace("\"", "&quot;");
        snprintf(buf, len, "<tr><td>%s</td><td class=r><button onclick=\"forget(%d,%s)\">Forget</button></td></tr>",
                 escaped, i, js.c_str());
        server.sendContent(buf);
    }
    if (wifiNetworkCount() == 0) server.sendContent("<tr><td>None saved yet</td></tr>");
    snprintf(buf, len, "</table><div class=row><select id=ssid onchange=ssidChanged()></select>"
                       "<input id=otherssid placeholder=\"Network name\" style=display:none>"
                       "<button onclick=scan()>Look again</button></div>"
                       "<div class=row><input id=pass type=password placeholder=Password autocomplete=off>"
                       "<button onclick=saveWifi()>Save network</button></div>"
                       "<p class=note>zvukopis joins the strongest saved network in reach when you open the web interface "
                       "or sync the clock (up to %d networks). Without one in reach it opens its own hotspot, "
                       "<b>%s</b>, with the password shown on its screen.</p>",
             MAX_NETWORKS, wifiHotspotName());
    server.sendContent(buf);
}

static void handleIndex() {
    noteRequest();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    server.sendContent_P(PAGE_HEAD);

    char buf[2048]; // a moment's row carries its category icon as SVG
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
    bool firstTime = info.hotspot && wifiNetworkCount() == 0;
    snprintf(buf, sizeof(buf), "<div class=sub>Device clock %s%s</div>"
                               "<div class=tabs><button id=tab-live onclick=\"showTab('live')\">Live</button>"
                               "<button id=tab-settings onclick=\"showTab('settings')\">Settings</button></div>"
                               "<script>const firstTab='%s'</script><div id=live class=tab>",
             clock, batteryText, firstTime ? "settings" : "live");
    server.sendContent(buf);
    // Live: recording and the moments
    sendRecord(buf, sizeof(buf));
    sendMoments(buf, sizeof(buf));
    server.sendContent("</div><div id=settings class=tab>");
    // Settings, most used first; Wi-Fi first only for someone setting the device up
    if (firstTime) sendWifi(buf, sizeof(buf));
    sendSchedule(buf, sizeof(buf));
    sendCategories(buf, sizeof(buf));
    sendTimeZone(buf, sizeof(buf));
    if (!firstTime) sendWifi(buf, sizeof(buf));
    server.sendContent("</div>");
    server.sendContent_P(PAGE_TAIL);
    server.sendContent("");
}

static void handleScheduleAdd() {
    noteRequest();
    Schedule s = {};
    tm local = {};
    int hour = 0, minute = 0, second = 0;
    if (sscanf(server.arg("date").c_str(), "%d-%d-%d", &local.tm_year, &local.tm_mon, &local.tm_mday) != 3 ||
        sscanf(server.arg("time").c_str(), "%d:%d:%d", &hour, &minute, &second) < 2) {
        server.send(400, "text/plain", "Choose a date, and a time as HH:MM or HH:MM:SS");
        return;
    }
    local.tm_year -= 1900;
    local.tm_mon -= 1;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    local.tm_isdst = -1; // the zone's rule decides
    s.start = mktime(&local);
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

static void handleRecordStart() {
    noteRequest();
    AudioState a = audioState();
    if (a == AUDIO_RECORDING || a == AUDIO_SAVING) {
        server.send(409, "text/plain", "Already recording");
        return;
    }
    if (!uiCanRecord() || audioLongestRecording() < 1) {
        server.send(409, "text/plain", "No room to record (memory or card full)");
        return;
    }
    if (a == AUDIO_PLAYING) audioStopPlayback();
    uint32_t seconds = server.arg("seconds").toInt();
    audioStartRecording(seconds > 0 ? seconds : 24 * 3600); // otherwise until stopped or memory runs out
    server.send(200, "text/plain", "ok");
}

static void handleGain() {
    noteRequest();
    int db = server.arg("db").toInt();
    bool known = false;
    for (int step : GAIN_STEPS) known |= step == db;
    if (!known) {
        server.send(400, "text/plain", "Bad gain");
        return;
    }
    audioSetMicGain(db);
    server.send(200, "text/plain", "ok");
}

static void handleRecordStop() {
    noteRequest();
    if (audioState() == AUDIO_RECORDING) audioStopRecording();
    server.send(200, "text/plain", "ok");
}

// Polled every second by the page; not counted as use, so an open tab doesn't keep Wi-Fi on forever
static void handleStatus() {
    const char *state = "idle";
    switch (audioState()) {
    case AUDIO_RECORDING: state = "recording"; break;
    case AUDIO_SAVING: state = "saving"; break;
    case AUDIO_PLAYING: state = "playing"; break;
    default: break;
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"seconds\":%.1f,\"limit\":%.0f,\"moments\":%d,\"gain\":%d,\"gainSetting\":%d}",
             state, audioRecordingSeconds(), audioRecordingLimitSeconds(), momentCount(), audioMicGain(), audioMicGainSetting());
    server.send(200, "application/json", json);
}

static void handleTimeZone() {
    noteRequest();
    String tz = server.arg("tz");
    tz.trim();
    if (tz.length() == 0 || tz.length() > 60) {
        server.send(400, "text/plain", "That doesn't look like a time zone rule");
        return;
    }
    clockSetTimeZone(tz.c_str());
    server.send(200, "text/plain", "ok");
}

static void handleWifiScan() {
    noteRequest();
    // In hotspot mode this scans from its second (station) interface
    int found = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < found; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || json.indexOf(jsonString(ssid)) >= 0) continue; // hidden, or seen on another channel
        if (json.length() > 1) json += ",";
        json += "{\"ssid\":" + jsonString(ssid) + ",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    WiFi.scanDelete();
    server.send(200, "application/json", json + "]");
}

static void handleWifiSave() {
    noteRequest();
    WifiNetwork n = {};
    String ssid = server.arg("ssid"), password = server.arg("password");
    if (ssid.length() == 0 || ssid.length() >= sizeof(n.ssid) || password.length() >= sizeof(n.password) ||
        (password.length() > 0 && password.length() < 8)) {
        server.send(400, "text/plain", "Enter the network's name, and its password (8 characters or more; empty for an open network)");
        return;
    }
    strlcpy(n.ssid, ssid.c_str(), sizeof(n.ssid));
    strlcpy(n.password, password.c_str(), sizeof(n.password));
    if (!wifiNetworkSave(n)) {
        server.send(400, "text/plain", "Already 4 networks saved; forget one first");
        return;
    }
    server.send(200, "text/plain", "ok");
}

static void handleWifiForget() {
    noteRequest();
    wifiNetworkForget(server.arg("i").toInt());
    server.send(200, "text/plain", "ok");
}

// Phones and laptops check for a captive portal by fetching a known page;
// sending them here makes them open the zvukopis page by themselves
static void redirectToPage() {
    server.sendHeader("Location", String("http://") + info.address + "/");
    server.send(302, "text/plain", "");
}

static void handleMoment();
static void handleTime();
static void handleDelete();

static void addRoutes() {
    server.on("/", HTTP_GET, handleIndex);
    server.on("/moment", HTTP_GET, handleMoment);
    server.on("/time", HTTP_POST, handleTime);
    server.on("/delete", HTTP_POST, handleDelete);
    server.on("/schedule/add", HTTP_POST, handleScheduleAdd);
    server.on("/schedule/remove", HTTP_POST, handleScheduleRemove);
    server.on("/categories", HTTP_POST, handleCategories);
    server.on("/timezone", HTTP_POST, handleTimeZone);
    server.on("/record/start", HTTP_POST, handleRecordStart);
    server.on("/record/stop", HTTP_POST, handleRecordStop);
    server.on("/gain", HTTP_POST, handleGain);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/wifi/save", HTTP_POST, handleWifiSave);
    server.on("/wifi/forget", HTTP_POST, handleWifiForget);
    for (const char *probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                              "/connecttest.txt", "/ncsi.txt", "/success.txt", "/canonical.html"}) {
        server.on(probe, redirectToPage);
    }
    server.onNotFound([] {
        if (info.hotspot) redirectToPage();
        else server.send(404, "text/plain", "Not found");
    });
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
    snprintf(disposition, sizeof(disposition), "%s; filename=\"%s\"", server.hasArg("play") ? "inline" : "attachment", name);
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

static bool stopping() { return stopRequested; }

static bool joinNetwork() {
    char ssid[33];
    if (wifiJoin(ssid, sizeof(ssid), stopping) != WIFI_JOINED) return false;
    strlcpy(info.network, ssid, sizeof(info.network));
    info.password[0] = 0;
    strlcpy(info.address, WiFi.localIP().toString().c_str(), sizeof(info.address));
    info.hotspot = false;
    return true;
}

static void openHotspot() {
    WiFi.mode(WIFI_AP_STA); // the station side scans for networks to set up
    WiFi.softAP(wifiHotspotName(), wifiHotspotPassword());
    strlcpy(info.network, wifiHotspotName(), sizeof(info.network));
    strlcpy(info.password, wifiHotspotPassword(), sizeof(info.password));
    strlcpy(info.address, WiFi.softAPIP().toString().c_str(), sizeof(info.address));
    info.hotspot = true;
    dns.start(53, "*", WiFi.softAPIP());
}

static void transferTask(void *) {
    static bool routesAdded = false;
    bool joined = !hotspotOnly && joinNetwork();
    if (!stopRequested) {
        if (joined) clockNtp(); // already online, so the clock gets corrected too
        else openHotspot();
        Serial.printf("[transfer] serving http://%s/ on %s\n", info.address, info.network);

        MDNS.begin(HOST_NAME);
        MDNS.addService("http", "tcp", 80);
        if (!routesAdded) addRoutes();
        routesAdded = true;
        server.begin();
        lastRequestAt = millis();
        info.state = TRANSFER_READY;

        while (!stopRequested && millis() - lastRequestAt < IDLE_TIMEOUT_MS) {
            if (info.hotspot) dns.processNextRequest();
            server.handleClient();
            delay(2);
        }
        server.stop();
        if (info.hotspot) dns.stop();
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
