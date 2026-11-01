#include "wifi_web.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <Update.h>
#include <esp_mac.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "catalog.h"
#include "clock.h"
#include "config.h"
#include "logger.h"
#include "settings.h"
#include "telemetry.h"

#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 4
#define RESTART_DELAY_MS 1500 // czas na wysłanie odpowiedzi i zamknięcie logu
#define OTA_STALL_MS 15000    // brak danych przez tyle = przerwana aktualizacja (np. telefon się rozłączył)

static AsyncWebServer server(80);
static bool active = false;
static bool server_started = false;
static char ssid[24] = "";
static char ip_text[16] = "";

// Ustawienia z przeglądarki czekają na zadanie UI (LVGL i strony ekranu nie są bezpieczne między zadaniami)
struct WebSettings {
    uint8_t live_count;
    uint16_t live_ids[LIVE_MAX];
    uint16_t chart_series[CHART_SERIES_MAX];
    uint8_t csv_format;
};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static WebSettings pending;
static volatile bool pending_set = false;

static volatile int ota_progress = -1;
static volatile bool ota_ok = false;
static volatile bool ota_failed = false;
static volatile uint32_t ota_last_data_ms = 0;
static size_t ota_written = 0;
static size_t ota_total = 0;
static volatile bool restart_pending = false;
static uint32_t restart_ms = 0;
static const char *volatile message = NULL;

// Strona WWW (jeden plik, bez zewnętrznych zasobów - telefon nie ma internetu w sieci ESP32)
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Diamel Telemetry</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#111;color:#eee}
header{display:flex;justify-content:space-between;align-items:center;padding:14px 16px;background:#1b1b1b;border-bottom:1px solid #333;font-weight:600}
#st{font-size:13px;font-weight:400;color:#999}#st.rec{color:#ef5350}#st.off{color:#ff9800}
nav{display:flex;gap:8px;padding:10px 16px}
nav button{flex:1;padding:10px 4px;border:0;border-radius:8px;background:#2a2a2a;color:#eee;font-size:15px}
nav button.on{background:#ff9800;color:#111}
main{padding:0 16px 24px;max-width:900px;margin:0 auto}
section{display:none}section.on{display:block}
.row{display:flex;align-items:center;gap:8px;padding:9px 0;border-bottom:1px solid #2a2a2a}
.row .n{flex:1;word-break:break-all}.row .s{color:#999;font-size:13px}
a.btn,button.btn{padding:8px 12px;border:0;border-radius:6px;background:#2a2a2a;color:#eee;text-decoration:none;font-size:14px;cursor:pointer}
button.btn:disabled{opacity:.35}
button.del{background:#5d1f1f}button.pri{background:#ff9800;color:#111;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin-top:6px}
.card{background:#1b1b1b;border-radius:8px;padding:10px}
.card .l{color:#999;font-size:13px}.card .v{font-size:26px;font-variant-numeric:tabular-nums}
.card .u{color:#999;font-size:14px;margin-left:4px}
.g1 .l{color:#64b5f6}.g2 .l{color:#80cbc4}
h3{color:#ff9800;font-size:14px;margin:22px 0 6px;letter-spacing:.05em}
label.chk{display:flex;gap:8px;align-items:center;margin:8px 0;color:#bbb}
input[type=search],select{width:100%;box-sizing:border-box;padding:9px;border-radius:6px;border:1px solid #333;background:#1b1b1b;color:#eee;font-size:15px}
select{margin:4px 0}
.hint{color:#888;font-size:13px;margin:6px 0}
#found .row{cursor:pointer}
progress{width:100%;height:18px}
.msg{margin:10px 0;color:#81c784}.msg.err{color:#ef5350}
</style></head><body>
<header><span>Diamel Telemetry</span><span id="st">...</span></header>
<nav><button data-t="live" class="on">Na zywo</button><button data-t="files">Pliki</button><button data-t="set">Ustawienia</button><button data-t="sys">System</button></nav>
<main>
<section id="live" class="on">
<div class="grid" id="cards"></div>
</section>
<section id="files">
<p class="hint" id="card"></p>
<div id="flist"></div>
<p><button class="btn del" id="delall">Usun wszystkie logi</button> <button class="btn" id="frefresh">Odswiez</button></p>
<h3>KARTA SD</h3>
<p class="hint">Formatowanie (FAT32) usuwa wszystkie dane z karty, z wyjatkiem katalogu parametrow /diamel.</p>
<p><button class="btn del" id="format">Formatuj karte</button></p>
<div id="fmsg" class="msg"></div>
</section>
<section id="set">
<h3>ODCZYTY (KOLEJNOSC)</h3>
<div id="sel"></div>
<p class="hint" id="selcnt"></p>
<h3>WYKRES</h3>
<div id="chart"></div>
<p class="hint">Inna jednostka niz Seria 1 = prawa os wykresu.</p>
<h3>DODAJ PARAMETR</h3>
<select id="mod"></select>
<input type="search" id="q" placeholder="Szukaj po nazwie...">
<div id="found"></div>
<h3>FORMAT LOGU CSV</h3>
<select id="csv"><option value="0">Excel PL (srednik, przecinek dziesietny)</option><option value="1">Standard (przecinek, kropka dziesietna)</option></select>
<p><button class="btn pri" id="save">Zapisz w urzadzeniu</button> <button class="btn" id="undo">Cofnij zmiany</button></p>
<div id="smsg" class="msg"></div>
</section>
<section id="sys">
<h3>URZADZENIE</h3>
<p id="about" class="hint"></p>
<h3>KATALOG PARAMETROW (KARTA SD /diamel)</h3>
<p class="hint">Pliki .csv z folderu docs/private/diamel_f31 (zaznacz wszystkie). Plik o tej samej nazwie zostanie zastapiony.</p>
<div id="clist"></div>
<input type="file" id="cfiles" accept=".csv" multiple>
<p><button class="btn pri" id="cupload">Wgraj pliki</button> <button class="btn del" id="cclear">Usun katalog</button></p>
<progress id="cprog" max="100" value="0" hidden></progress>
<div id="cmsg" class="msg"></div>
<h3>AKTUALIZACJA OPROGRAMOWANIA</h3>
<p class="hint">Plik firmware.bin z katalogu .pio/build/esp32-s3 projektu. Po wgraniu urzadzenie uruchomi sie ponownie, a Wi-Fi bedzie wylaczone.</p>
<input type="file" id="fw" accept=".bin">
<p><button class="btn pri" id="upload">Wgraj</button></p>
<progress id="prog" max="100" value="0" hidden></progress>
<div id="umsg" class="msg"></div>
</section>
</main>
<script>
const $=s=>document.querySelector(s);
// Parametr: [indeks, nazwa, jednostka, miejsca, modul]; M = moduly [indeks, nazwa, liczba]
let M=[],S=null,E=null,tab='live',timer=null,cardKey='';
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}
async function j(u){const r=await fetch(u);if(!r.ok)throw new Error(r.status);return r.json()}
async function post(u,o){const r=await fetch(u,{method:'POST',body:new URLSearchParams(o)});if(!r.ok)throw new Error(await r.text()||r.status);return r}
function fmt(v,d){return v==null?'--':v.toFixed(d)}
function size(b){return b<1024?b+' B':b<1048576?(b/1024).toFixed(0)+' KB':(b/1048576).toFixed(1)+' MB'}
function status(t,c){const s=$('#st');s.textContent=t;s.className=c||''}

function show(t){tab=t;
 document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('on',b.dataset.t==t));
 document.querySelectorAll('section').forEach(s=>s.classList.toggle('on',s.id==t));
 if(t=='live')live();if(t=='files')files();if(t=='set')editStart();if(t=='sys')catalog();}
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>show(b.dataset.t));

// --- Na zywo ---
function modName(m){const x=M.find(a=>a[0]==m);return x?x[1]:''}
function buildCards(){const c=$('#cards');c.innerHTML='';
 if(!S.live.length)c.append(el('p','hint','Brak wybranych wartosci - zakladka Ustawienia.'));
 S.live.forEach(p=>{const d=el('div','card'+(p[4]?' g1':''));d.append(el('div','l',p[1]));
  const v=el('div');const n=el('span','v','--');n.id='v'+p[0];v.append(n,el('span','u',p[2]));d.append(v);c.append(d)})}
async function live(){clearTimeout(timer);if(tab!='live')return;
 try{const ids=S.live.map(p=>p[0]),key=ids.join(',');
  const d=await j('/api/live?ids='+key);
  if(key!=cardKey){buildCards();cardKey=key}
  ids.forEach((i,k)=>{const e=document.getElementById('v'+i);if(e)e.textContent=d.v[k]});
  if(d.rec)status('REC '+d.rec,'rec');else status(d.demo?'DEMO':'polaczono')}
 catch(e){status('brak polaczenia','off')}
 timer=setTimeout(live,500)}

// --- Pliki ---
let fmtWait=false;
async function files(){const l=$('#flist');
 try{const d=await j('/api/files');l.innerHTML='';
  $('#format').disabled=d.fmt;
  if(d.fmt){$('#card').textContent='Trwa formatowanie karty...';$('#delall').hidden=true;fmtWait=true;setTimeout(files,1000);return}
  if(fmtWait){fmtWait=false;const m=$('#fmsg');m.className=d.fmtok&&!d.fmterr?'msg':'msg err';m.textContent=d.fmterr||(d.fmtok?'Karta sformatowana, katalog /diamel zachowany.':'Blad formatowania karty.')}
  if(!d.sd){$('#card').textContent='Brak karty SD lub karta nieczytelna.';$('#delall').hidden=true;return}
  $('#card').textContent='Wolne '+d.free+' z '+d.total+' MB, logi: '+d.files.length+(d.other?', inne dane: '+d.other+' MB':'');
  $('#delall').hidden=!d.files.length;
  if(!d.files.length)l.append(el('p','hint','Brak logow. Nagrywanie: przycisk REC na ekranie.'));
  d.files.forEach(f=>{const r=el('div','row'),rec=f[0]==d.rec;
   r.append(el('span','n',f[0]),el('span','s',rec?'zapis...':size(f[1])));
   const a=el('a','btn','Pobierz');a.href='/dl?f='+encodeURIComponent(f[0]);
   const x=el('button','btn del','Usun');x.disabled=rec;x.onclick=()=>del(f[0]);
   r.append(a,x);l.append(r)})}
 catch(e){$('#card').textContent='Blad: '+e.message}}
async function del(n){if(!confirm('Usunac '+n+'?'))return;try{await post('/api/delete',{f:n})}catch(e){alert('Nie mozna usunac: '+e.message)}files()}
$('#delall').onclick=async()=>{if(!confirm('Usunac wszystkie logi?'))return;try{await post('/api/delete_all',{})}catch(e){alert(e.message)}files()};
$('#frefresh').onclick=files;
$('#format').onclick=async()=>{const m=$('#fmsg');
 if(!confirm('Sformatowac karte? Wszystkie dane poza katalogiem /diamel zostana usuniete.'))return;
 try{await post('/api/format',{});m.className='msg';m.textContent='Formatowanie...';fmtWait=true}
 catch(e){m.className='msg err';m.textContent=e.message}
 files()};

// --- Ustawienia ---
function editStart(){E={live:[...S.live],chart:[...S.chart],csv:S.csv};$('#smsg').textContent='';renderEdit()}
function renderEdit(){const s=$('#sel');s.innerHTML='';
 E.live.forEach((p,k)=>{const r=el('div','row');r.append(el('span','n',(k+1)+'. '+p[1]),el('span','s',modName(p[4])));
  const up=el('button','btn','↑'),dn=el('button','btn','↓'),x=el('button','btn del','✕');
  up.disabled=k==0;dn.disabled=k==E.live.length-1;
  up.onclick=()=>{[E.live[k-1],E.live[k]]=[E.live[k],E.live[k-1]];renderEdit()};
  dn.onclick=()=>{[E.live[k+1],E.live[k]]=[E.live[k],E.live[k+1]];renderEdit()};
  x.onclick=()=>{E.live.splice(k,1);renderEdit()};
  r.append(up,dn,x);s.append(r)});
 $('#selcnt').textContent='Wybrane: '+E.live.length+' / '+S.max;
 const c=$('#chart');c.innerHTML='';
 E.chart.forEach((p,k)=>{const r=el('div','row');r.append(el('span','n','Seria '+(k+1)+': '+(p?p[1]:'(brak)')));
  if(p){const x=el('button','btn del','✕');x.onclick=()=>{E.chart[k]=null;renderEdit()};r.append(x)}
  c.append(r)});
 $('#csv').value=E.csv;search(0)}
// Wyszukiwanie po stronie urzadzenia (katalog ma tysiace parametrow), po 50 wynikow
let sTimer=null;
async function search(off){const f=$('#found');if(!off)f.innerHTML='';
 const q=$('#q').value.trim(),m=$('#mod').value;
 if(!q&&m==-1){f.append(el('p','hint','Wybierz modul albo wpisz nazwe.'));return}
 try{const d=await j('/api/search?m='+m+'&off='+off+'&q='+encodeURIComponent(q));
  const more=f.querySelector('.more');if(more)more.remove();
  d.items.forEach(p=>{const r=el('div','row');
   r.append(el('span','n',p[1]+(p[2]?' ['+p[2]+']':'')),el('span','s',m==-1?modName(p[4]):''));
   const a=el('button','btn','+ Odczyty');a.disabled=E.live.length>=S.max||E.live.some(x=>x[0]==p[0]);
   a.onclick=()=>{E.live.push(p);renderEdit()};r.append(a);
   [0,1,2].forEach(k=>{const b=el('button','btn','S'+(k+1));b.title='Seria '+(k+1)+' wykresu';
    b.onclick=()=>{E.chart[k]=p;renderEdit()};r.append(b)});
   f.append(r)});
  if(!d.total)f.append(el('p','hint','Nic nie znaleziono.'));
  const shown=off+d.items.length;
  if(shown<d.total){const b=el('button','btn more','Pokaz wiecej ('+shown+' z '+d.total+')');b.onclick=()=>search(shown);f.append(b)}}
 catch(e){f.append(el('p','hint','Blad: '+e.message))}}
$('#q').oninput=()=>{clearTimeout(sTimer);sTimer=setTimeout(()=>search(0),300)};
$('#mod').onchange=()=>search(0);
function fillModules(){const s=$('#mod');s.innerHTML='';
 const o=el('option',null,'Wszystkie moduly');o.value=-1;s.append(o);
 M.forEach(m=>{const op=el('option',null,m[1]+' ('+m[2]+')');op.value=m[0];s.append(op)})}
$('#csv').onchange=()=>{E.csv=+$('#csv').value};
$('#undo').onclick=editStart;
$('#save').onclick=async()=>{const m=$('#smsg');
 try{await post('/api/settings',{live:E.live.map(p=>p[0]).join(','),chart:E.chart.map(p=>p?p[0]:-1).join(','),csv:E.csv});
  S=await j('/api/settings');cardKey='';m.className='msg';m.textContent='Zapisano.'}
 catch(e){m.className='msg err';m.textContent='Blad: '+e.message}};

// --- Katalog parametrow na karcie SD ---
async function catalog(){const l=$('#clist');
 try{const d=await j('/api/catalog');l.innerHTML='';
  if(!d.sd){l.append(el('p','hint','Brak karty SD.'));return}
  if(!d.files.length)l.append(el('p','hint','Katalog pusty.'));
  d.files.forEach(f=>{const r=el('div','row');r.append(el('span','n',f[0]),el('span','s',size(f[1])));l.append(r)})}
 catch(e){l.textContent='Blad: '+e.message}}
$('#cupload').onclick=()=>{const fs=$('#cfiles').files,m=$('#cmsg'),pr=$('#cprog');
 if(!fs.length){m.className='msg err';m.textContent='Wybierz pliki .csv';return}
 const fd=new FormData();for(const f of fs)fd.append('f',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/api/catalog');
 x.upload.onprogress=e=>{if(e.lengthComputable)pr.value=e.loaded*100/e.total};
 x.onload=()=>{m.className=x.status==200?'msg':'msg err';m.textContent=x.status==200?'Wgrano plikow: '+fs.length+'. Uruchom urzadzenie ponownie (Info > Uruchom ponownie), aby wczytac katalog.':'Blad: '+x.responseText;$('#cupload').disabled=false;catalog()};
 x.onerror=()=>{m.className='msg err';m.textContent='Przerwano polaczenie.';$('#cupload').disabled=false};
 pr.hidden=false;pr.value=0;m.textContent='';$('#cupload').disabled=true;x.send(fd)};
$('#cclear').onclick=async()=>{if(!confirm('Usunac caly katalog parametrow z karty?'))return;
 try{await post('/api/catalog_clear',{})}catch(e){alert(e.message)}catalog()};

// --- System / OTA ---
$('#upload').onclick=()=>{const f=$('#fw').files[0],m=$('#umsg'),pr=$('#prog');
 if(!f){m.className='msg err';m.textContent='Wybierz plik .bin';return}
 if(!confirm('Wgrac '+f.name+' ('+size(f.size)+')?'))return;
 const fd=new FormData();fd.append('firmware',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{if(e.lengthComputable)pr.value=e.loaded*100/e.total};
 x.onload=()=>{if(x.status==200){m.className='msg';m.textContent='Wgrano. Urzadzenie uruchamia sie ponownie - wlacz Wi-Fi na ekranie i odswiez strone.'}
  else{m.className='msg err';m.textContent='Blad aktualizacji: '+x.responseText}$('#upload').disabled=false};
 x.onerror=()=>{m.className='msg err';m.textContent='Przerwano polaczenie.';$('#upload').disabled=false};
 pr.hidden=false;pr.value=0;m.textContent='';$('#upload').disabled=true;x.send(fd)};

(async()=>{try{
 // Zegar urzadzenia z czasu przegladarki (plytka nie ma zegara z bateria); czas lokalny
 post('/api/time',{t:Math.floor(Date.now()/1000)-new Date().getTimezoneOffset()*60}).catch(()=>{});
 const p=await j('/api/params');M=p.modules;S=await j('/api/settings');
 $('#about').textContent='Wersja '+p.fw+', pojazd: '+p.vehicle+(p.demo?', tryb DEMO':'')+'. Katalog: '+p.catalog;
 fillModules();live()}catch(e){status('brak polaczenia','off');setTimeout(()=>location.reload(),3000)}})();
</script></body></html>)HTML";

// --- Pomocnicze ---
static void send_json(AsyncWebServerRequest *req, const JsonDocument &doc) {
    AsyncResponseStream *res = req->beginResponseStream("application/json");
    res->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *res);
    req->send(res);
}

static String form_value(AsyncWebServerRequest *req, const char *name) {
    const AsyncWebParameter *p = req->getParam(name, true);
    return p ? p->value() : String();
}

// Lista liczb oddzielonych przecinkami; -1 = PARAM_NONE. Zwraca liczbę pozycji albo -1 przy błędzie.
static int parse_ids(const String &text, uint16_t *out, int max, bool allow_none) {
    const char *s = text.c_str();
    int n = 0;
    while (*s) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || n >= max) return -1;
        if (v == -1 && allow_none) out[n++] = PARAM_NONE;
        else if (v >= 0 && v < params_count()) out[n++] = (uint16_t)v;
        else return -1;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return n;
}

// --- Obsługa zapytań ---
// Parametr dla strony: [indeks, nazwa, jednostka, miejsca po przecinku, moduł]
static void add_param(JsonArray arr, ParamIdx id) {
    const ParamDef &p = param(id);
    JsonArray a = arr.add<JsonArray>();
    a.add(id);
    a.add(p.name);
    a.add(p.unit);
    a.add(p.decimals);
    a.add(p.module);
}

// Informacje o urządzeniu i lista modułów (sam katalog jest za duży - przeszukuje go /api/search)
static void handle_params(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    doc["vehicle"] = FW_VEHICLE;
    doc["demo"] = g_settings.data_source == SRC_DEMO;
    CatalogStatus cs;
    catalog_status(cs);
    char text[80];
    if (cs.loaded) snprintf(text, sizeof(text), "%u parametrow z %u modulow", (unsigned)cs.params, cs.modules);
    else snprintf(text, sizeof(text), "nie wczytano (%s)", cs.error[0] ? cs.error : "brak danych");
    doc["catalog"] = text;
    JsonArray mods = doc["modules"].to<JsonArray>();
    for (int m = 0; m < modules_count(); m++) {
        if (!module_def(m).count) continue;
        JsonArray a = mods.add<JsonArray>();
        a.add(m);
        a.add(module_def(m).name);
        a.add(module_def(m).count);
    }
    send_json(req, doc);
}

// Czas z przeglądarki (sekundy, już w strefie lokalnej) - ustawia zegar urządzenia
static void handle_time(AsyncWebServerRequest *req) {
    long long t = atoll(form_value(req, "t").c_str());
    if (t < 1704067200LL) return req->send(400, "text/plain", "Zly czas"); // przed 2024
    clock_set((time_t)t);
    char buf[24];
    clock_format(buf, sizeof(buf));
    Serial.printf("Zegar: ustawiony z przegladarki: %s\n", buf);
    req->send(200, "text/plain", buf);
}

static void handle_search(AsyncWebServerRequest *req) {
    static ParamIdx found[50]; // AsyncTCP obsługuje zapytania po kolei - jeden bufor wystarcza
    int module = req->hasParam("m") ? req->getParam("m")->value().toInt() : -1;
    int offset = req->hasParam("off") ? max(0L, req->getParam("off")->value().toInt()) : 0;
    String q = req->hasParam("q") ? req->getParam("q")->value() : String();
    int total = params_search(module, q.c_str(), offset, found, 50);
    JsonDocument doc;
    doc["total"] = total;
    JsonArray items = doc["items"].to<JsonArray>();
    for (int i = 0; i < min(50, max(0, total - offset)); i++) add_param(items, found[i]);
    send_json(req, doc);
}

// Wartości wybranych parametrów (?ids=1,2,3) jako gotowe teksty - jak na ekranie
static void handle_live(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["demo"] = g_settings.data_source == SRC_DEMO;
    doc["rec"] = logger_current_file();
    JsonArray v = doc["v"].to<JsonArray>();
    String ids = req->hasParam("ids") ? req->getParam("ids")->value() : String();
    const char *s = ids.c_str();
    int n = 0;
    while (*s && n < LIVE_MAX + CHART_SERIES_MAX) {
        char *end;
        long id = strtol(s, &end, 10);
        if (end == s) break;
        char buf[48] = "--";
        float val;
        if (id >= 0 && id < params_count() && telemetry_value(id, val)) {
            const char *text = param_value_text(id, val);
            if (text) strlcpy(buf, text, sizeof(buf));
            else snprintf(buf, sizeof(buf), "%.*f", param(id).decimals, val);
        }
        v.add(buf);
        n++;
        s = *end == ',' ? end + 1 : end;
    }
    send_json(req, doc);
}

static void handle_get_settings(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["max"] = LIVE_MAX;
    JsonArray live = doc["live"].to<JsonArray>();
    for (int i = 0; i < g_settings.live_count; i++) add_param(live, g_settings.live_ids[i]);
    JsonArray chart = doc["chart"].to<JsonArray>();
    for (int i = 0; i < CHART_SERIES_MAX; i++) {
        uint16_t id = g_settings.chart_series[i];
        if (id == PARAM_NONE) chart.add(nullptr);
        else add_param(chart, id);
    }
    doc["csv"] = g_settings.csv_format;
    send_json(req, doc);
}

static void handle_post_settings(AsyncWebServerRequest *req) {
    WebSettings w;
    memset(&w, 0, sizeof(w));
    int n = parse_ids(form_value(req, "live"), w.live_ids, LIVE_MAX, false);
    if (n < 0) return req->send(400, "text/plain", "Bledna lista odczytow");
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < i; k++) {
            if (w.live_ids[k] == w.live_ids[i]) return req->send(400, "text/plain", "Powtorzona wartosc");
        }
    }
    w.live_count = n;
    if (parse_ids(form_value(req, "chart"), w.chart_series, CHART_SERIES_MAX, true) != CHART_SERIES_MAX) {
        return req->send(400, "text/plain", "Bledne serie wykresu");
    }
    long csv = form_value(req, "csv").toInt();
    if (csv < 0 || csv >= CSV_COUNT) return req->send(400, "text/plain", "Bledny format CSV");
    w.csv_format = (uint8_t)csv;

    portENTER_CRITICAL(&mux);
    pending = w;
    pending_set = true;
    portEXIT_CRITICAL(&mux);
    req->send(200, "text/plain", "OK");
}

static void handle_files(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["sd"] = logger_sd_ok();
    doc["fmt"] = logger_formatting();
    doc["fmtok"] = logger_format_ok();
    doc["fmterr"] = logger_format_error();
    if (logger_sd_ok()) {
        const uint64_t MB = 1024 * 1024;
        uint64_t total = SD.totalBytes();
        uint64_t used = SD.usedBytes();
        uint64_t logs_bytes = 0;
        doc["total"] = (uint32_t)(total / MB);
        doc["free"] = (uint32_t)((total - used) / MB);
        doc["rec"] = logger_current_file();
        JsonArray files = doc["files"].to<JsonArray>();
        File dir = SD.open(LOG_DIR);
        if (dir) {
            for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
                if (!f.isDirectory()) {
                    JsonArray e = files.add<JsonArray>();
                    e.add(f.name());
                    e.add((uint32_t)f.size());
                    logs_bytes += f.size();
                }
                f.close();
            }
            dir.close();
        }
        doc["other"] = (uint32_t)(used > logs_bytes ? (used - logs_bytes) / MB : 0); // dane spoza logów
    }
    send_json(req, doc);
}

// --- Katalog parametrów (/diamel): lista, wgrywanie wielu plików naraz, usuwanie ---
#define CATALOG_TMP CATALOG_DIR "/_upload.tmp"

static AsyncWebServerRequest *cat_req = nullptr; // zapytanie, którego pliki właśnie przychodzą
static File cat_file;
static String cat_target;
static bool cat_error = false;
static int cat_count = 0;

static void catalog_list(JsonArray files) {
    File dir = SD.open(CATALOG_DIR);
    if (!dir) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            JsonArray e = files.add<JsonArray>();
            e.add(f.name());
            e.add((uint32_t)f.size());
        }
        f.close();
    }
    dir.close();
}

static void handle_catalog(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["sd"] = logger_sd_ok();
    JsonArray files = doc["files"].to<JsonArray>();
    if (logger_sd_ok()) catalog_list(files);
    send_json(req, doc);
}

// Każdy plik najpierw do pliku tymczasowego - przerwane wgrywanie nie psuje działającego katalogu
static void handle_catalog_upload(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data,
                                  size_t len, bool final) {
    if (req != cat_req) { // pierwszy plik nowego zapytania
        cat_req = req;
        cat_error = false;
        cat_count = 0;
    }
    if (index == 0) {
        if (cat_file) cat_file.close();
        bool ok = logger_sd_ok() && logger_valid_name(filename.c_str()) && filename.endsWith(".csv");
        if (ok) {
            if (!SD.exists(CATALOG_DIR)) SD.mkdir(CATALOG_DIR);
            cat_target = String(CATALOG_DIR) + "/" + filename;
            cat_file = SD.open(CATALOG_TMP, FILE_WRITE);
        }
        if (!ok || !cat_file) {
            Serial.printf("Katalog: odrzucono %s\n", filename.c_str());
            cat_error = true;
            return;
        }
    }
    if (!cat_file) return;
    if (len && cat_file.write(data, len) != len) {
        cat_file.close();
        SD.remove(CATALOG_TMP);
        cat_error = true;
        return;
    }
    if (final) {
        cat_file.close();
        SD.remove(cat_target);
        if (SD.rename(CATALOG_TMP, cat_target)) cat_count++;
        else cat_error = true;
        Serial.printf("Katalog: %s, %u B\n", cat_target.c_str(), (unsigned)(index + len));
    }
}

static void handle_catalog_done(AsyncWebServerRequest *req) {
    if (cat_file) cat_file.close();
    if (cat_error) req->send(500, "text/plain", "Nie wszystkie pliki zapisano (tylko .csv, nazwa do 23 znakow, karta SD)");
    else req->send(200, "text/plain", String(cat_count));
    cat_req = nullptr;
}

static void handle_catalog_clear(AsyncWebServerRequest *req) {
    if (!logger_sd_ok()) return req->send(409, "text/plain", "Brak karty SD");
    static char names[64][24];
    int count = 0;
    File dir = SD.open(CATALOG_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && count < 64; f = dir.openNextFile()) {
            if (!f.isDirectory()) strlcpy(names[count++], f.name(), sizeof(names[0]));
            f.close();
        }
        dir.close();
    }
    for (int i = 0; i < count; i++) SD.remove(String(CATALOG_DIR) + "/" + names[i]);
    req->send(200, "text/plain", "OK");
}

static void handle_format(AsyncWebServerRequest *req) {
    if (logger_format_start()) req->send(200, "text/plain", "OK");
    else req->send(409, "text/plain", "Najpierw zatrzymaj nagrywanie (albo formatowanie juz trwa)");
}

static void handle_download(AsyncWebServerRequest *req) {
    const AsyncWebParameter *p = req->getParam("f");
    if (!p || !logger_sd_ok() || !logger_valid_name(p->value().c_str())) return req->send(400, "text/plain", "Zla nazwa");
    String path = String(LOG_DIR) + "/" + p->value();
    if (!SD.exists(path)) return req->send(404, "text/plain", "Brak pliku");
    req->send(SD, path, "text/csv", true); // true = pobieranie jako plik
}

static void handle_delete(AsyncWebServerRequest *req) {
    if (logger_delete(form_value(req, "f").c_str())) req->send(200, "text/plain", "OK");
    else req->send(409, "text/plain", "Plik w trakcie zapisu albo blad karty");
}

static void handle_delete_all(AsyncWebServerRequest *req) {
    if (!logger_sd_ok()) return req->send(409, "text/plain", "Brak karty SD");
    // Najpierw lista, potem usuwanie - bez zmieniania katalogu w trakcie jego czytania
    static char names[200][24];
    int count = 0;
    File dir = SD.open(LOG_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && count < 200; f = dir.openNextFile()) {
            if (!f.isDirectory()) strlcpy(names[count++], f.name(), sizeof(names[0]));
            f.close();
        }
        dir.close();
    }
    for (int i = 0; i < count; i++) logger_delete(names[i]); // plik w trakcie zapisu zostaje
    req->send(200, "text/plain", "OK");
}

// --- Aktualizacja OTA (plik firmware.bin w formularzu) ---
static void handle_update_upload(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data,
                                 size_t len, bool final) {
    if (index == 0) {
        ota_ok = false;
        ota_failed = false;
        ota_written = 0;
        ota_total = req->contentLength(); // z narzutem formularza - postęp jest przybliżony
        ota_progress = 0;
        Serial.printf("OTA: start %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            ota_failed = true;
        }
    }
    ota_last_data_ms = millis();
    if (!ota_failed && len) {
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            ota_failed = true;
        }
        ota_written += len;
        if (ota_total) ota_progress = min(99, (int)(ota_written * 100 / ota_total));
    }
    if (final) {
        if (!ota_failed && Update.end(true)) {
            ota_ok = true;
            ota_progress = 100;
            Serial.printf("OTA: OK, %u B\n", (unsigned)ota_written);
        } else {
            if (!ota_failed) Update.printError(Serial);
            Update.abort();
            ota_failed = true;
            ota_progress = -1;
        }
    }
}

static void handle_update_done(AsyncWebServerRequest *req) {
    if (ota_ok) {
        req->send(200, "text/plain", "OK");
        message = "Aktualizacja OK - restart";
        restart_pending = true;
    } else {
        req->send(500, "text/plain", Update.hasError() ? Update.errorString() : "Plik nieprawidlowy");
        message = "Blad aktualizacji";
        ota_progress = -1;
    }
}

static void setup_routes() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html; charset=utf-8", (const uint8_t *)INDEX_HTML, sizeof(INDEX_HTML) - 1);
    });
    server.on("/api/params", HTTP_GET, handle_params);
    server.on("/api/live", HTTP_GET, handle_live);
    server.on("/api/search", HTTP_GET, handle_search);
    server.on("/api/time", HTTP_POST, handle_time);
    server.on("/api/settings", HTTP_GET, handle_get_settings);
    server.on("/api/settings", HTTP_POST, handle_post_settings);
    server.on("/api/files", HTTP_GET, handle_files);
    server.on("/dl", HTTP_GET, handle_download);
    server.on("/api/delete", HTTP_POST, handle_delete);
    server.on("/api/delete_all", HTTP_POST, handle_delete_all);
    server.on("/api/format", HTTP_POST, handle_format);
    server.on("/api/catalog", HTTP_GET, handle_catalog);
    server.on("/api/catalog", HTTP_POST, handle_catalog_done, handle_catalog_upload);
    server.on("/api/catalog_clear", HTTP_POST, handle_catalog_clear);
    server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
    server.onNotFound([](AsyncWebServerRequest *req) { req->send(404, "text/plain", "Nie znaleziono"); });
}

// --- Sterowanie ---
const char *wifi_ssid() {
    if (!ssid[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(ssid, sizeof(ssid), "Diamel-%02X%02X", mac[4], mac[5]);
    }
    return ssid;
}

bool wifi_start() {
    if (active) return true;
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(wifi_ssid(), g_settings.wifi_pass, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
        Serial.println("Wi-Fi: BLAD uruchomienia punktu dostepowego");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    strlcpy(ip_text, WiFi.softAPIP().toString().c_str(), sizeof(ip_text));
    if (!server_started) {
        setup_routes();
        server.begin();
        server_started = true;
    }
    active = true;
    Serial.printf("Wi-Fi: siec %s, adres http://%s, wolny RAM wewn.: %u KB\n", ssid, ip_text,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

void wifi_stop() {
    if (!active) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    active = false;
    ip_text[0] = 0;
    Serial.println("Wi-Fi: wylaczone");
}

bool wifi_active() { return active; }

uint8_t wifi_clients() { return active ? WiFi.softAPgetStationNum() : 0; }

const char *wifi_ip() { return ip_text; }

int wifi_ota_progress() { return ota_progress; }

bool wifi_apply_web_settings() {
    if (!pending_set) return false;
    portENTER_CRITICAL(&mux);
    WebSettings w = pending;
    pending_set = false;
    portEXIT_CRITICAL(&mux);

    g_settings.live_count = w.live_count;
    memcpy(g_settings.live_ids, w.live_ids, sizeof(w.live_ids));
    memcpy(g_settings.chart_series, w.chart_series, sizeof(w.chart_series));
    g_settings.csv_format = w.csv_format;
    settings_mark_dirty();
    return true;
}

void wifi_tick() {
    // Telefon rozłączył się w trakcie wysyłania pliku - zwalniamy aktualizację
    int p = ota_progress;
    if (p >= 0 && p < 100 && millis() - ota_last_data_ms > OTA_STALL_MS) {
        Update.abort();
        ota_failed = true;
        ota_progress = -1;
        message = "Aktualizacja przerwana";
    }

    if (!restart_pending) return;
    if (!restart_ms) {
        restart_ms = millis();
        logger_stop(); // zadanie logu zamknie plik w ciągu jednego interwału
    }
    if (millis() - restart_ms > RESTART_DELAY_MS && !logger_active()) {
        settings_flush();
        Serial.println("OTA: restart");
        Serial.flush();
        ESP.restart();
    }
}

const char *wifi_take_message() {
    const char *m = message;
    message = NULL;
    return m;
}
