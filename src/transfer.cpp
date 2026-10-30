#include "transfer.h"

#include <Arduino.h>
#include <esp_rom_crc.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "board_power.h"
#include "clock.h"
#include "config.h"
#include "library.h"
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
    if (moment.fileName[0]) strlcpy(buf, moment.fileName, len);
    else storageMomentFileName(moment, buf, len); // held in memory: the name it would have on the card
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
.big{width:100%;padding:.85rem;font-size:1.25rem;font-weight:600;border-radius:.7rem;background:#111;color:#fff;border:1px solid #111;
display:flex;justify-content:center;align-items:center;gap:.6rem}.big:disabled{opacity:.5;cursor:default}
.big .dot{width:.9rem;height:.9rem;border-radius:50%;background:#e4312b;display:inline-block}.big.rec .dot{border-radius:2px;background:#fff}
.mono{font-variant-numeric:tabular-nums}.opts{display:flex;gap:1.2rem;flex-wrap:wrap;margin:.7rem 0 .2rem}.opts label{display:inline-flex;gap:.4rem;align-items:center}
.ic{border:1px solid #999;background:none;color:inherit;border-radius:.45rem;width:2.2rem;height:2.2rem;display:inline-flex;align-items:center;justify-content:center;padding:0;cursor:pointer}
.ic svg{width:1rem;height:1rem;fill:currentColor}td.c{width:2.4rem;padding-left:0;padding-right:.25rem}
table{width:100%;border-collapse:collapse}td{padding:.55rem .3rem;border-top:1px solid #ddd;vertical-align:middle}
td.r{text-align:right;white-space:nowrap}a,button,input,select{font:inherit}
.icon{vertical-align:-4px}.pick{display:inline-flex;gap:2px;flex-wrap:wrap}
.pick label{cursor:pointer;padding:5px;border-radius:.4rem;border:1px solid transparent;line-height:0}
.pick input{display:none}.pick input:checked+svg{color:#fff;background:#111;border-radius:.3rem;box-shadow:0 0 0 4px #111}
button{border:1px solid #111;background:#fff;color:#111;border-radius:.4rem;padding:.3rem .7rem;cursor:pointer}
input,select{border:1px solid #888;border-radius:.4rem;padding:.25rem .4rem;background:inherit;color:inherit;max-width:100%}
.tag{font-size:.85rem;border:1px solid #888;border-radius:1rem;padding:0 .45rem;white-space:nowrap}
.row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:.5rem 0}
.cal{margin:.6rem 0 .2rem;max-width:22rem}.cal .head{display:flex;align-items:center;justify-content:space-between;margin-bottom:.3rem}
.cal .head b{font-size:1.05rem}.cal .grid{display:grid;grid-template-columns:repeat(7,1fr);gap:3px}
.cal .wd{text-align:center;font-size:.75rem;color:#777;padding:.2rem 0}
.cal .day{aspect-ratio:1;border:1px solid transparent;border-radius:.45rem;background:none;color:inherit;padding:0;display:flex;flex-direction:column;align-items:center;justify-content:center;line-height:1.1;cursor:default}
.cal .day.has{border-color:#999;cursor:pointer}.cal .day.has small{font-size:.7rem;color:#555}.cal .day.on{background:#111;color:#fff}
.cal .day.on small{color:#ccc}.cal .day.off{opacity:.35}
@media (prefers-color-scheme:dark){.cal .day.on{background:#eee;color:#111}.cal .day.on small{color:#444}.cal .day.has small{color:#aaa}}
input[type=number]{width:4.5rem}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}td{border-color:#333}.tabs{border-color:#333}
.pick input:checked+svg{color:#111;background:#eee;box-shadow:0 0 0 4px #eee}
.big{background:#eee;color:#111;border-color:#eee}.big.rec .dot{background:#111}
button{background:#111;color:#eee;border-color:#eee}.sub,small,.note{color:#aaa}a{color:#9cf}}
</style></head><body><h1>zvukopis</h1>
)";

static const char PAGE_TAIL[] PROGMEM = R"(
<script>
const $=id=>document.getElementById(id);
const t=s=>Math.floor(s/60)+':'+String(Math.floor(s%60)).padStart(2,'0');
const PLAY='<svg viewBox="0 0 16 16"><path d="M3 1.5v13l11-6.5z"/></svg>',STOP='<svg viewBox="0 0 16 16"><rect x="2.5" y="2.5" width="11" height="11" rx="1.5"/></svg>';
async function post(u,body){const r=await fetch(u,{method:'POST',body:body});if(!r.ok)alert(await r.text());location.reload()}
function setClock(){post('/time?ms='+Date.now())}
function showTab(t){for(const n of ['live','settings']){$(n).classList.toggle('on',n===t);$('tab-'+n).classList.toggle('on',n===t)}
 const h=t==='live'?'#recording':'#settings';if(location.hash!==h)history.replaceState(null,'',h)}
showTab(location.hash==='#settings'?'settings':location.hash==='#live'||location.hash==='#recording'?'live':firstTab);
function del(n){if(confirm('Delete Moment '+n+'? This can\'t be undone.'))post('/delete?n='+n)}
// Calendar of the days with moments; a day filters the list and can be downloaded on its own
const DAYS={};for(const r of document.querySelectorAll('#moments tr[data-day]')){const d=r.dataset.day;if(/^\d{4}-\d\d-\d\d$/.test(d))DAYS[d]=(DAYS[d]||0)+1}
let calMonth=null,calDay=null;
const MONTHS=['January','February','March','April','May','June','July','August','September','October','November','December'];
function calInit(){const c=$('cal');if(!c)return;const ds=Object.keys(DAYS).sort();if(!ds.length){c.style.display='none';return}
 let keep=null;try{keep=sessionStorage.getItem('calDay')}catch(e){}
 calMonth=ds[ds.length-1].slice(0,7);if(keep&&DAYS[keep]){calPick(keep)}else calDraw()}
function calMove(n){const [y,m]=calMonth.split('-').map(Number);const d=new Date(y,m-1+n,1);
 calMonth=d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0');calDraw()}
function calDraw(){const [y,m]=calMonth.split('-').map(Number);const first=new Date(y,m-1,1),days=new Date(y,m,0).getDate();
 let h='<div class=head><button onclick=calMove(-1)>&#8249;</button><b>'+MONTHS[m-1]+' '+y+'</b><button onclick=calMove(1)>&#8250;</button></div><div class=grid>';
 for(const w of ['Mo','Tu','We','Th','Fr','Sa','Su'])h+='<div class=wd>'+w+'</div>';
 for(let i=0;i<(first.getDay()+6)%7;i++)h+='<div></div>';
 for(let d=1;d<=days;d++){const k=calMonth+'-'+String(d).padStart(2,'0'),n=DAYS[k];
  h+=n?'<button class="day has'+(k===calDay?' on':'')+'" data-k='+k+'>'+d+'<small>'+n+'</small></button>':'<div class="day off">'+d+'</div>'}
 $('cal').innerHTML=h+'</div>';$('cal').onclick=e=>{const b=e.target.closest('.has');if(b)calPick(b.dataset.k)}}
function calPick(k){calDay=calDay===k?null:k;try{if(calDay)sessionStorage.setItem('calDay',calDay);else sessionStorage.removeItem('calDay')}catch(e){}
 calMonth=calDay?calDay.slice(0,7):calMonth;calDraw();
 for(const r of document.querySelectorAll('#moments tr[data-day]'))r.style.display=!calDay||r.dataset.day===calDay?'':'none';
 const row=$('dayrow');if(!calDay){row.style.display='none';return}
 const [y,m,d]=calDay.split('-');const n=DAYS[calDay];
 row.style.display='';row.innerHTML='<b>'+Number(d)+' '+MONTHS[m-1]+' '+y+'</b><span class=sub>'+n+' moment'+(n==1?'':'s')+'</span>'+
  '<a href="/backup?day='+calDay+'" download><button>Download this day</button></a><button onclick=calPick(calDay)>Show all days</button>'}
calInit();
let playing=null;
function stopped(){if(playing===null)return;$('pb-'+playing).innerHTML=PLAY;$('pb-'+playing).title='Play';
 const d=$('dur-'+playing);d.textContent=d.dataset.len;playing=null}
function togglePlay(n){const p=$('player');
 if(playing===n){p.pause();p.currentTime=0;stopped();return}
 if(playing!==null)stopped();
 playing=n;p.src='/moment?n='+n+'&play=1';p.play();$('pb-'+n).innerHTML=STOP;$('pb-'+n).title='Stop';
 p.ontimeupdate=()=>{if(playing===n)$('dur-'+n).textContent=t(p.currentTime)+' / '+t(p.duration||0)};
 p.onended=stopped}
async function setGain(){const r=await fetch('/gain?db='+$('gain').value,{method:'POST'});if(!r.ok)alert(await r.text());poll()}
function addSchedule(){
 const d=$('date').value,tm=$('time').value.trim();
 if(!d){alert('Choose a date');return}
 if(!/^([01]?[0-9]|2[0-3]):[0-5][0-9](:[0-5][0-9])?$/.test(tm)){alert('Write the time as 24-hour HH:MM, for example 06:30');return}
 post('/schedule/add?date='+d+'&time='+tm+'&seconds='+$('len').value+'&daily='+($('daily').checked?1:0))}
function unschedule(s){post('/schedule/remove?start='+s)}
function saveCategories(){
 const q=new URLSearchParams();
 for(let i=1;i<=5;i++){q.set('name'+i,$('name'+i).value);q.set('icon'+i,document.querySelector('input[name=icon'+i+']:checked').value)}
 post('/categories?'+q)}
function zoneChanged(){$('customtz').style.display=$('tz').value==='custom'?'':'none';$('savetz').disabled=false}
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
async function upload(){const f=$('docfile').files[0];if(!f){alert('Choose a .md or .txt file first');return}
 const fd=new FormData();fd.append('doc',f,f.name);$('upbtn').disabled=true;
 const r=await fetch('/library/upload',{method:'POST',body:fd});if(!r.ok)alert(await r.text());location.reload()}
function deldoc(f,t){if(confirm('Delete '+t+' from the device?'))post('/library/delete?f='+encodeURIComponent(f))}
let recState='idle',moments=null;
async function toggleRecord(){
 const r=await fetch(recState=='recording'?'/record/stop':'/record/start?seconds='+$('reclen').value,{method:'POST'});
 if(!r.ok)alert(await r.text());poll()}
async function poll(){
 try{const st=await(await fetch('/status')).json();
  recState=st.state;const b=$('recbtn');b.disabled=st.state=='saving';b.classList.toggle('rec',st.state=='recording');
  $('reclabel').textContent=st.state=='recording'?'Stop':st.state=='saving'?'Saving...':'Record';
  $('rectime').textContent=st.state=='recording'?t(st.seconds)+(st.limit<86400?' / '+t(st.limit):''):'';
  $('gainnow').textContent=st.gainSetting<0?'(now '+st.gain+' dB)':'';
  if(moments!==null&&st.moments!=moments)location.reload();moments=st.moments;
 }catch(e){}}
setInterval(poll,1000);poll();
if($('ssid')&&firstTab==='settings')scan();else if($('ssid'))ssidChanged();
</script></body></html>)";

static const int GAIN_STEPS[] = {GAIN_AUTO, 18, 24, 30, 36, 42, 48, 54};
static const int LENGTHS[] = {30, 60, 120, 300, 600, 1800, 3600, 7200, 14400, 28800, 43200};

// "12 h", "1 h 30 min", "5 min", "45 s"
static void lengthText(char *buf, size_t len, int seconds) {
    if (seconds >= 3600 && seconds % 3600 >= 60) snprintf(buf, len, "%d h %d min", seconds / 3600, seconds % 3600 / 60);
    else if (seconds >= 3600) snprintf(buf, len, "%d h", seconds / 3600);
    else if (seconds >= 60 && seconds % 60) snprintf(buf, len, "%d min %d s", seconds / 60, seconds % 60);
    else if (seconds >= 60) snprintf(buf, len, "%d min", seconds / 60);
    else snprintf(buf, len, "%d s", seconds);
}

// The length presets up to `longest`, as <option>s
static void sendLengthOptions(char *buf, size_t len, int longest, int selected) {
    bool any = false;
    for (int seconds : LENGTHS) {
        if (seconds > longest) break;
        char text[24];
        lengthText(text, sizeof(text), seconds);
        snprintf(buf, len, "<option value=%d%s>%s</option>", seconds, seconds == selected ? " selected" : "", text);
        server.sendContent(buf);
        any = true;
    }
    if (!any) {
        snprintf(buf, len, "<option value=%d>%d s</option>", max(1, longest), max(1, longest));
        server.sendContent(buf);
    }
}

static void sendRecord(char *buf, size_t len) {
    // One button that records and stops, showing the time as it goes
    server.sendContent("<h2>Record</h2><button id=recbtn class=big onclick=toggleRecord()>"
                       "<span class=dot></span><span id=reclabel>Record</span><span id=rectime class=mono></span></button>"
                       "<div class=opts><label>Stop after <select id=reclen><option value=0>when I say</option>");
    int longest = (int)audioLongestRecording();
    sendLengthOptions(buf, len, longest, 0);
    int setting = audioMicGainSetting();
    server.sendContent("</select></label><label>Mic gain <select id=gain onchange=setGain()>");
    for (int db : GAIN_STEPS) {
        if (db == GAIN_AUTO) snprintf(buf, len, "<option value=%d%s>Auto</option>", db, setting == db ? " selected" : "");
        else snprintf(buf, len, "<option value=%d%s>%d dB</option>", db, setting == db ? " selected" : "", db);
        server.sendContent(buf);
    }
    char longestText[24];
    lengthText(longestText, sizeof(longestText), longest);
    snprintf(buf, len, "</select><span id=gainnow class=sub></span></label></div>"
                       "<p class=note>Up to %s at a time%s. Wi-Fi stays on while recording, which may add a little noise.</p>",
             longestText, audioUsingSdCard() ? " (the card's room)" : " (no SD card: held in memory)");
    server.sendContent(buf);
}

// The day a moment belongs to, as in its folder on the card: "2026-10-28",
// or "undated" without a clock
static void momentDay(const Moment &moment, char *buf, size_t len) {
    if (moment.folder[0]) strlcpy(buf, moment.folder, len);
    else if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        strftime(buf, len, "%Y-%m-%d", &local);
    } else strlcpy(buf, "undated", len);
}

static void sendMoments(char *buf, size_t len) {
    int count = momentCount();
    uint64_t bytes = 0;
    for (int i = 0; i < count; i++) {
        Moment moment;
        if (momentGet(i, moment)) bytes += WAV_HEADER_BYTES + moment.length * sizeof(int16_t);
    }
    if (count) snprintf(buf, len, "<h2>Moments (%d)</h2>", count);
    else snprintf(buf, len, "<h2>Moments</h2>");
    server.sendContent(buf);
    if (count) {
        // A calendar of the days with moments (built by the script from the
        // rows below), and the whole lot as one ZIP
        char size[16];
        if (bytes >= 1000000000ull) snprintf(size, sizeof(size), "%.1f GB", bytes / 1e9);
        else snprintf(size, sizeof(size), "%lu MB", (unsigned long)((bytes + 500000) / 1000000));
        bool fits = bytes + (uint64_t)count * 300 < 0xFFFF0000ull; // the ZIP's own headers; ZIP64 isn't done
        if (fits) snprintf(buf, len, "<div id=cal class=cal></div><div id=dayrow class=row style=\"display:none\"></div>"
                                     "<div class=row><a href=\"/backup\" download><button>Download all</button></a>"
                                     "<span class=note>Every moment as WAV files in one ZIP, about %s, in day folders</span></div>", size);
        else snprintf(buf, len, "<div id=cal class=cal></div><div id=dayrow class=row style=\"display:none\"></div>"
                                "<p class=note>About %s of moments: more than one ZIP can hold, so download a day at a time from the calendar.</p>", size);
        server.sendContent(buf);
    }
    server.sendContent("<audio id=player></audio><table id=moments>");
    for (int i = 0; i < count; i++) {
        Moment moment;
        if (!momentGet(i, moment)) break;
        char when[32] = "", name[48], tag[1024] = "", day[12];
        momentDay(moment, day, sizeof(day));
        if (moment.recordedAt) {
            tm local;
            localtime_r(&moment.recordedAt, &local);
            strftime(when, sizeof(when), "%e %b %Y %H:%M:%S", &local);
        }
        downloadName(moment, name, sizeof(name));
        uint32_t rate = moment.sampleRate ? moment.sampleRate : SAMPLE_RATE;
        int seconds = (int)((moment.length + rate / 2) / rate);
        if (moment.tag != TAG_NONE) {
            Category c = configCategory(moment.tag);
            char escaped[CATEGORY_NAME_LEN * 6];
            htmlEscape(c.name, escaped, sizeof(escaped));
            snprintf(tag, sizeof(tag), " <span class=tag>%s %s</span>", iconSvg(c.icon, 14).c_str(), escaped);
        }
        snprintf(buf, len,
                 "<tr data-day=\"%s\"><td class=c><button class=ic id=pb-%d title=Play onclick=togglePlay(%d)>"
                 "<svg viewBox=\"0 0 16 16\"><path d=\"M3 1.5v13l11-6.5z\"/></svg></button></td>"
                 "<td><b>Moment %d</b>%s<br><small>%s</small></td>"
                 "<td class=\"r mono\" id=dur-%d data-len=\"%d:%02d\">%d:%02d</td>"
                 "<td class=c><a class=ic href=\"/moment?n=%d\" download=\"%s\" title=Download>"
                 "<svg viewBox=\"0 0 16 16\"><path d=\"M7 1h2v7.2l2.6-2.6 1.4 1.4L8 12 3 7l1.4-1.4L7 8.2zM2 12h12v2H2z\"/></svg></a></td>"
                 "<td class=c><button class=ic title=Delete onclick=del(%d)>"
                 "<svg viewBox=\"0 0 16 16\"><path d=\"M6 1h4l.5 1H14v2H2V2h3.5zM3 5h10l-.8 10H3.8z\"/></svg></button></td></tr>",
                 day, moment.number, moment.number, moment.number, tag, when, moment.number, seconds / 60, seconds % 60,
                 seconds / 60, seconds % 60, moment.number, name, moment.number);
        server.sendContent(buf);
    }
    if (count == 0) server.sendContent("<tr><td>No moments yet. Press Record to capture one.</td></tr>");
    server.sendContent("</table>");
}

static const char TRASH_SVG[] = "<svg viewBox=\"0 0 16 16\"><path d=\"M6 1h4l.5 1H14v2H2V2h3.5zM3 5h10l-.8 10H3.8z\"/></svg>";

static void sendSchedule(char *buf, size_t len) {
    int n = scheduleCount();
    if (n) snprintf(buf, len, "<h2>Scheduled recordings (%d)</h2>", n);
    else snprintf(buf, len, "<h2>Scheduled recordings</h2>");
    server.sendContent(buf);
    if (!clockValid()) server.sendContent("<p class=note>The device clock isn't set, so these can't run on time. Set it under Time first.</p>");
    server.sendContent("<table>");
    Schedule s;
    for (int i = 0; scheduleGet(i, s); i++) {
        time_t start = (time_t)s.start;
        tm local;
        localtime_r(&start, &local);
        char when[40], length[16];
        strftime(when, sizeof(when), "%a %e %b, %H:%M", &local);
        if (s.seconds % 60 == 0) snprintf(length, sizeof(length), "%lu min", (unsigned long)(s.seconds / 60));
        else snprintf(length, sizeof(length), "%lu:%02lu", (unsigned long)(s.seconds / 60), (unsigned long)(s.seconds % 60));
        snprintf(buf, len, "<tr><td><b>%s</b>%s<br><small>%s%s</small></td>"
                           "<td class=c><button class=ic title=Remove onclick=unschedule(%lld)>%s</button></td></tr>",
                 s.daily ? "Every day" : when, s.daily ? "" : "", length, s.daily ? (strftime(when, sizeof(when), ", at %H:%M, next %a %e %b", &local), when) : "",
                 (long long)s.start, TRASH_SVG);
        server.sendContent(buf);
    }
    if (n == 0) server.sendContent("<tr><td>None yet. Add one below: zvukopis will wake up and record at that time, even from sleep.</td></tr>");
    // The form starts at the next whole minute, in zvukopis's local time
    char today[12] = "", nextMinute[8] = "";
    if (clockValid()) {
        time_t t = time(nullptr) + 60;
        tm local;
        localtime_r(&t, &local);
        strftime(today, sizeof(today), "%Y-%m-%d", &local);
        strftime(nextMinute, sizeof(nextMinute), "%H:%M", &local);
    }
    snprintf(buf, len, "</table><div class=row><input type=date id=date value=\"%s\">"
                       "<input id=time size=5 placeholder=HH:MM value=\"%s\" inputmode=numeric><span>for</span><select id=len>",
             today, nextMinute);
    server.sendContent(buf);
    int longest = (int)audioLongestRecording();
    sendLengthOptions(buf, len, longest, 60);
    char longestText[24];
    lengthText(longestText, sizeof(longestText), longest);
    snprintf(buf, len, "</select><label><input type=checkbox id=daily> every day</label><button onclick=addSchedule()>Add</button></div>"
                       "<p class=note>24-hour time, in zvukopis's own time zone. Up to %s per recording.</p>",
             longestText);
    server.sendContent(buf);
}

static void sendCategories(char *buf, size_t len) {
    server.sendContent("<h2>Categories</h2><p class=note>What a moment can be tagged as, with a long press on zvukopis. "
                       "Renaming one renames it on the moments already tagged.</p>");
    for (int slot = 1; slot <= CATEGORY_COUNT; slot++) {
        Category c = configCategory(slot);
        char escaped[CATEGORY_NAME_LEN * 6];
        htmlEscape(c.name, escaped, sizeof(escaped));
        snprintf(buf, len, "<div class=row><input id=name%d maxlength=%d value=\"%s\" style=\"width:9rem\"><span class=pick>",
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
    server.sendContent("<div class=row><button onclick=saveCategories()>Save</button></div>");
}

static void sendTimeZone(char *buf, size_t len) {
    const char *current = clockTimeZone();
    bool known = false;
    char clock[40] = "not set";
    if (clockValid()) {
        time_t now = time(nullptr);
        tm local;
        localtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%a %e %b %Y, %H:%M:%S", &local);
    }
    snprintf(buf, len, "<h2>Time</h2><div class=row><span>Device clock: <b>%s</b></span>"
                       "<button onclick=setClock()>Set from this browser</button></div>"
                       "<div class=row><span>Time zone</span><select id=tz onchange=zoneChanged()>", clock);
    server.sendContent(buf);
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
                       "<input id=customtz size=24 value=\"%s\" style=\"%s\">"
                       "<button id=savetz onclick=saveZone() disabled>Save</button></div>"
                       "<div class=row><button onclick=useBrowserZone()>Use this browser's zone</button></div>"
                       "<p class=note>Syncing the clock over the internet is in zvukopis's own Settings. Daylight saving follows the zone by itself.</p>",
             known ? "" : " selected", escaped, known ? "display:none" : "");
    server.sendContent(buf);
}

static void sendLibrary(char *buf, size_t len) {
    int n = libraryCount();
    if (n) snprintf(buf, len, "<h2>Library (%d)</h2>", n);
    else snprintf(buf, len, "<h2>Library</h2>");
    server.sendContent(buf);
    if (!audioUsingSdCard()) {
        server.sendContent("<p class=note>Documents live on the SD card; there is none in zvukopis right now.</p>");
        return;
    }
    server.sendContent("<table>");
    Document doc;
    for (int i = 0; libraryGet(i, doc); i++) {
        char title[CATEGORY_NAME_LEN * 0 + 300], name[300];
        htmlEscape(doc.title, title, sizeof(title));
        htmlEscape(doc.fileName, name, sizeof(name));
        // Into a JS string inside an HTML attribute: JSON-escape, then HTML-escape the lot
        char jsName[400], jsTitle[400];
        htmlEscape(jsonString(doc.fileName).c_str(), jsName, sizeof(jsName));
        htmlEscape(jsonString(doc.title).c_str(), jsTitle, sizeof(jsTitle));
        snprintf(buf, len, "<tr><td><b>%s</b><br><small>%s, %lu KB</small></td>"
                           "<td class=c><button class=ic title=Delete onclick=\"deldoc(%s,%s)\">%s</button></td></tr>",
                 title, name, (unsigned long)((doc.bytes + 1023) / 1024), jsName, jsTitle, TRASH_SVG);
        server.sendContent(buf);
    }
    if (n == 0) server.sendContent("<tr><td>Nothing to read yet. Add a Markdown or text file below.</td></tr>");
    server.sendContent("</table><div class=row><input type=file id=docfile accept=\".md,.txt,text/markdown,text/plain\">"
                       "<button id=upbtn onclick=upload()>Upload</button></div>"
                       "<p class=note>Markdown or plain text, up to 512 KB each. Headings, paragraphs, <i>italics</i>, "
                       "<b>bold</b>, quotations and lists are shown; they go in the <code>doc</code> folder on the card, "
                       "and zvukopis remembers the page you left each one at.</p>");
}

static void sendWifi(char *buf, size_t len) {
    bool firstTime = info.hotspot && wifiNetworkCount() == 0;
    int n = wifiNetworkCount();
    if (firstTime) snprintf(buf, len, "<h2>Connect zvukopis to your Wi-Fi</h2>");
    else if (n) snprintf(buf, len, "<h2>Wi-Fi networks (%d)</h2>", n);
    else snprintf(buf, len, "<h2>Wi-Fi networks</h2>");
    server.sendContent(buf);
    server.sendContent("<table>");
    WifiNetwork net;
    for (int i = 0; wifiNetworkGet(i, net); i++) {
        char escaped[200];
        htmlEscape(net.ssid, escaped, sizeof(escaped));
        String js = jsonString(net.ssid);
        js.replace("\"", "&quot;");
        snprintf(buf, len, "<tr><td>%s</td><td class=c><button class=ic title=Forget onclick=\"forget(%d,%s)\">%s</button></td></tr>",
                 escaped, i, js.c_str(), TRASH_SVG);
        server.sendContent(buf);
    }
    if (n == 0) server.sendContent("<tr><td>None saved yet. Choose yours below and enter its password.</td></tr>");
    // No scan on load: it blocks the server for seconds. The button runs one.
    snprintf(buf, len, "</table><div class=row><select id=ssid onchange=ssidChanged()><option value=\"\">Type a name, or press the arrows to look</option></select>"
                       "<button class=ic title=\"Look for networks again\" onclick=scan()>"
                       "<svg viewBox=\"0 0 16 16\"><path d=\"M8 2a6 6 0 0 1 5.2 3H11v2h5V2h-2v1.7A8 8 0 0 0 8 0zM8 14a6 6 0 0 1-5.2-3H5V9H0v5h2v-1.7A8 8 0 0 0 8 16z\"/></svg></button>"
                       "<input id=otherssid placeholder=\"Network name\" style=display:none></div>"
                       "<div class=row><input id=pass type=password placeholder=Password autocomplete=off>"
                       "<button onclick=saveWifi()>Save network</button></div>"
                       "<p class=note>zvukopis joins the strongest saved network (up to %d) when you open the web interface "
                       "or sync the clock. With none in reach it opens its own hotspot, <b>%s</b>; the password is on its screen.</p>",
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
                               "<div class=tabs><button id=tab-live onclick=\"showTab('live')\">Recording</button>"
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
    sendLibrary(buf, sizeof(buf));
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

// Documents arrive in pieces; the file is written as they come
static File uploadFile;
static String uploadError;

static String uploadPath;
static size_t uploadBytes = 0;

// A failed upload leaves nothing behind
static void dropUpload(const char *why) {
    uploadError = why;
    if (uploadFile) uploadFile.close();
    if (uploadPath.length()) SD_MMC.remove(uploadPath);
    uploadPath = "";
}

static void handleLibraryUploadPiece() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadError = "";
        uploadPath = "";
        uploadBytes = 0;
        if (!audioUsingSdCard()) {
            uploadError = "No SD card in zvukopis";
            return;
        }
        // A plain file name: letters, digits, dots, dashes, spaces; .md or .txt
        String name = up.filename;
        int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
        if (slash >= 0) name = name.substring(slash + 1);
        String clean;
        for (unsigned i = 0; i < name.length() && clean.length() < 40; i++) {
            char c = name[i];
            if (isalnum((uint8_t)c) || c == '.' || c == '-' || c == '_' || c == ' ') clean += c;
        }
        clean.trim();
        if (!clean.endsWith(".md") && !clean.endsWith(".txt") && !clean.endsWith(".MD") && !clean.endsWith(".TXT")) clean += ".md";
        if (clean.length() < 4 || clean[0] == '.') {
            uploadError = "Give the file a plain name ending in .md or .txt";
            return;
        }
        if (!SD_MMC.exists(libraryDir())) SD_MMC.mkdir(libraryDir());
        uploadPath = String(libraryDir()) + "/" + clean;
        uploadFile = SD_MMC.open(uploadPath, FILE_WRITE);
        if (!uploadFile) dropUpload("Can't create the file on the SD card");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!uploadFile) return; // already failed
        // Counted here: the file's own size() reads the directory entry, which
        // FatFs only updates on close
        if (uploadBytes + up.currentSize > MAX_DOCUMENT_BYTES) dropUpload("Documents can be 512 KB at most");
        else if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) dropUpload("Writing to the SD card failed");
        else uploadBytes += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            if (uploadBytes == 0) dropUpload("The file is empty");
        }
        uploadPath = "";
        libraryScan();
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile || uploadPath.length()) dropUpload("The upload was cut off");
        libraryScan();
    }
    lastRequestAt = millis();
}

static void handleLibraryUpload() {
    noteRequest();
    if (uploadError.length()) server.send(400, "text/plain", uploadError);
    else server.send(200, "text/plain", "ok");
}

static void handleLibraryDelete() {
    noteRequest();
    if (!libraryDelete(server.arg("f").c_str())) {
        server.send(404, "text/plain", "No such document");
        return;
    }
    server.send(200, "text/plain", "ok");
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
static void handleBackup();

static void addRoutes() {
    server.on("/", HTTP_GET, handleIndex);
    server.on("/moment", HTTP_GET, handleMoment);
    server.on("/backup", HTTP_GET, handleBackup);
    server.on("/time", HTTP_POST, handleTime);
    server.on("/delete", HTTP_POST, handleDelete);
    server.on("/schedule/add", HTTP_POST, handleScheduleAdd);
    server.on("/schedule/remove", HTTP_POST, handleScheduleRemove);
    server.on("/categories", HTTP_POST, handleCategories);
    server.on("/timezone", HTTP_POST, handleTimeZone);
    server.on("/record/start", HTTP_POST, handleRecordStart);
    server.on("/record/stop", HTTP_POST, handleRecordStop);
    server.on("/gain", HTTP_POST, handleGain);
    server.on("/library/upload", HTTP_POST, handleLibraryUpload, handleLibraryUploadPiece);
    server.on("/library/delete", HTTP_POST, handleLibraryDelete);
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
    char name[48], disposition[80];
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

// --- Backup: every moment (or one day's) as WAV files in one ZIP ---
// Stored, not compressed (WAV doesn't shrink), streamed straight from the
// card. Each entry's CRC is known only after its data, so the local headers
// carry a data descriptor (flag bit 3) and the central directory at the end
// has the real numbers. ZIP64 isn't done: over 4 GB the page says to go by day.

struct ZipEntry {
    int number; // the moment's; the list may shift while the ZIP streams
    uint32_t size, crc, offset;
    uint16_t nameLen, dosTime, dosDate;
};

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// "moments/2026-10-28/2026-10-28T14-01-47_nature.wav", padded with '_' to
// `fixedLen` when given: the name's length was counted into Content-Length
// and a re-tag meanwhile could have changed it
static void zipEntryName(const Moment &moment, char *buf, size_t len, size_t fixedLen = 0) {
    char day[12], name[48];
    momentDay(moment, day, sizeof(day));
    downloadName(moment, name, sizeof(name));
    snprintf(buf, len, "moments/%s/%s", day, name);
    if (fixedLen) {
        size_t have = strlen(buf);
        if (have < fixedLen && fixedLen < len) memset(buf + have, '_', fixedLen - have);
        if (fixedLen < len) buf[fixedLen] = 0;
    }
}

static bool sendAll(WiFiClient &client, const uint8_t *data, size_t len) {
    for (size_t sent = 0; sent < len;) {
        if (!client.connected()) return false;
        size_t n = client.write(data + sent, len - sent);
        if (n == 0) return false;
        sent += n;
        lastRequestAt = millis();
    }
    return true;
}

static void handleBackup() {
    noteRequest();
    String day = server.arg("day");
    int count = momentCount();
    ZipEntry *entries = (ZipEntry *)ps_calloc(max(count, 1), sizeof(ZipEntry));
    if (!entries) {
        server.send(500, "text/plain", "Out of memory");
        return;
    }
    uint64_t total = 22; // end of central directory
    int n = 0;
    for (int i = 0; i < count; i++) {
        Moment moment;
        if (!momentGet(i, moment)) continue;
        char entryDay[12], name[80];
        momentDay(moment, entryDay, sizeof(entryDay));
        if (day.length() && day != entryDay) continue;
        zipEntryName(moment, name, sizeof(name));
        ZipEntry &e = entries[n++];
        e.number = moment.number;
        e.nameLen = strlen(name);
        if (moment.samples) {
            e.size = WAV_HEADER_BYTES + moment.length * sizeof(int16_t);
        } else {
            File f = storageOpenMoment(moment);
            e.size = f ? f.size() : 0;
            f.close();
        }
        tm local = {};
        if (moment.recordedAt) localtime_r(&moment.recordedAt, &local);
        else local.tm_year = 80; // 1980, the earliest ZIP date
        e.dosDate = ((max(local.tm_year - 80, 0)) << 9) | ((local.tm_mon + 1) << 5) | max(local.tm_mday, 1);
        e.dosTime = (local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2);
        total += 30 + e.nameLen + e.size + 16 + 46 + e.nameLen;
    }
    if (n == 0) {
        free(entries);
        server.send(404, "text/plain", "Nothing to download");
        return;
    }
    if (total >= 0xFFFF0000ull) { // ZIP64 isn't done, and the server's length sentinels sit just below 2^32
        free(entries);
        server.send(413, "text/plain", "Over 4 GB, too much for one ZIP: download the days one at a time");
        return;
    }

    char fileName[48], disposition[80];
    if (day.length()) snprintf(fileName, sizeof(fileName), "zvukopis-%s.zip", day.c_str());
    else {
        time_t now = time(nullptr);
        tm local;
        localtime_r(&now, &local);
        strftime(fileName, sizeof(fileName), "zvukopis-backup-%Y-%m-%d.zip", &local);
    }
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", fileName);
    server.sendHeader("Content-Disposition", disposition);
    server.setContentLength(total);
    server.send(200, "application/zip", "");
    WiFiClient client = server.client();
    Serial.printf("[transfer] backup of %d moment%s, %llu bytes%s\n", n, n == 1 ? "" : "s", total, day.length() ? " (one day)" : "");

    uint8_t *chunk = (uint8_t *)ps_malloc(SEND_CHUNK);
    uint8_t head[46 + 80], wav[WAV_HEADER_BYTES];
    uint32_t offset = 0;
    bool ok = chunk != nullptr;
    for (int k = 0; ok && k < n; k++) {
        ZipEntry &e = entries[k];
        Moment moment;
        int index;
        if (!findMoment(e.number, index, moment)) { ok = false; break; } // deleted meanwhile
        char name[80];
        zipEntryName(moment, name, sizeof(name), e.nameLen);
        e.offset = offset;
        // Local file header
        put32(head, 0x04034b50);
        put16(head + 4, 20);     // version needed
        put16(head + 6, 0x0008); // data descriptor follows
        put16(head + 8, 0);      // stored
        put16(head + 10, e.dosTime);
        put16(head + 12, e.dosDate);
        put32(head + 14, 0);     // crc, sizes: in the descriptor
        put32(head + 18, 0);
        put32(head + 22, 0);
        put16(head + 26, e.nameLen);
        put16(head + 28, 0);
        memcpy(head + 30, name, e.nameLen);
        ok = sendAll(client, head, 30 + e.nameLen);
        // The WAV itself, exactly e.size bytes (short reads are padded so the ZIP stays well-formed)
        uint32_t crc = 0, sent = 0;
        if (ok && moment.samples) {
            size_t headerSize = min(storageWavHeader(moment, wav), (size_t)e.size);
            crc = esp_rom_crc32_le(crc, wav, headerSize);
            ok = sendAll(client, wav, headerSize);
            sent = headerSize;
            const uint8_t *data = (const uint8_t *)moment.samples;
            size_t dataSize = moment.length * sizeof(int16_t);
            for (size_t at = 0; ok && at < dataSize && sent < e.size; at += SEND_CHUNK) {
                size_t len = min(min(SEND_CHUNK, dataSize - at), (size_t)(e.size - sent));
                crc = esp_rom_crc32_le(crc, data + at, len);
                ok = sendAll(client, data + at, len);
                sent += len;
            }
        } else if (ok) {
            File f = storageOpenMoment(moment);
            if (f) f.seek(0);
            while (ok && f && sent < e.size) {
                size_t len = f.read(chunk, min(SEND_CHUNK, (size_t)(e.size - sent)));
                if (len == 0) break;
                crc = esp_rom_crc32_le(crc, chunk, len);
                ok = sendAll(client, chunk, len);
                sent += len;
            }
            if (f) f.close();
        }
        if (ok && sent < e.size) {
            memset(chunk, 0, SEND_CHUNK);
            Serial.printf("[transfer] %s came up %lu bytes short\n", name, (unsigned long)(e.size - sent));
            while (ok && sent < e.size) {
                size_t len = min(SEND_CHUNK, (size_t)(e.size - sent));
                crc = esp_rom_crc32_le(crc, chunk, len);
                ok = sendAll(client, chunk, len);
                sent += len;
            }
        }
        e.crc = crc;
        // Data descriptor
        put32(head, 0x08074b50);
        put32(head + 4, crc);
        put32(head + 8, e.size);
        put32(head + 12, e.size);
        if (ok) ok = sendAll(client, head, 16);
        offset += 30 + e.nameLen + e.size + 16;
    }
    // Central directory
    uint32_t centralStart = offset, centralSize = 0;
    for (int k = 0; ok && k < n; k++) {
        ZipEntry &e = entries[k];
        Moment moment;
        int index;
        char name[80];
        memset(name, '_', sizeof(name));
        if (findMoment(e.number, index, moment)) zipEntryName(moment, name, sizeof(name), e.nameLen);
        put32(head, 0x02014b50);
        put16(head + 4, 20);
        put16(head + 6, 20);
        put16(head + 8, 0x0008);
        put16(head + 10, 0);
        put16(head + 12, e.dosTime);
        put16(head + 14, e.dosDate);
        put32(head + 16, e.crc);
        put32(head + 20, e.size);
        put32(head + 24, e.size);
        put16(head + 28, e.nameLen);
        put16(head + 30, 0); // extra
        put16(head + 32, 0); // comment
        put16(head + 34, 0); // disk
        put16(head + 36, 0); // internal attributes
        put32(head + 38, 0); // external attributes
        put32(head + 42, e.offset);
        memcpy(head + 46, name, e.nameLen);
        ok = sendAll(client, head, 46 + e.nameLen);
        centralSize += 46 + e.nameLen;
    }
    if (ok) {
        put32(head, 0x06054b50);
        put16(head + 4, 0);
        put16(head + 6, 0);
        put16(head + 8, n);
        put16(head + 10, n);
        put32(head + 12, centralSize);
        put32(head + 16, centralStart);
        put16(head + 20, 0);
        ok = sendAll(client, head, 22);
    }
    if (!ok) Serial.println("[transfer] backup cut off");
    free(chunk);
    free(entries);
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
