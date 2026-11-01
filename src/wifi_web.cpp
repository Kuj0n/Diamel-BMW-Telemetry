#include "wifi_web.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <Update.h>
#include <esp_mac.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "backup.h"
#include "catalog.h"
#include "clock.h"
#include "config.h"
#include "display.h"
#include "logger.h"
#include "settings.h"
#include "telemetry.h"

#define AP_CHANNEL 6
#define AP_MAX_CLIENTS 4
#define RESTART_DELAY_MS 1500 // czas na wysłanie odpowiedzi i zamknięcie logu
#define HOME_CONNECT_TIMEOUT_MS 20000 // sieć domowa niedostępna -> własna sieć
#define OTA_STALL_MS 15000    // brak danych przez tyle = przerwana aktualizacja (np. telefon się rozłączył)

static AsyncWebServer server(80);
static bool active = false;
static bool server_started = false;
static char ssid[24] = "";
static char ip_text[16] = "";
static volatile WifiState wstate = WSTATE_OFF;
static uint32_t connect_ms;
static uint32_t wifi_reconfig_ms; // zaplanowany restart Wi-Fi po zmianie sieci ze strony WWW

// Sieć domowa zapisana przez stronę WWW - do g_settings przenosi ją zadanie UI
struct WebWifi {
    uint8_t mode;
    char ssid[33];
    char pass[65];
    bool keep_pass;
};
static WebWifi pending_wifi;
static volatile bool pending_wifi_set = false;

// Ustawienia z przeglądarki czekają na zadanie UI (LVGL i strony ekranu nie są bezpieczne między zadaniami)
struct WebSettings {
    uint8_t page_count[LIVE_PAGES];
    uint16_t page_ids[LIVE_PAGES][PAGE_MAX];
    uint16_t chart_series[CHART_SERIES_MAX];
    uint8_t csv_format;
    uint8_t log_page;
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
.tabs{display:flex;gap:6px;margin:8px 0}.tabs button{flex:1}.tabs button.on{background:#ff9800;color:#111}
progress{width:100%;height:18px}
.msg{margin:10px 0;color:#81c784}.msg.err{color:#ef5350}
</style></head><body>
<header><span>Diamel Telemetry</span><span id="st">...</span></header>
<nav><button data-t="live" class="on">Na zywo</button><button data-t="set">Odczyty</button><button data-t="chartset">Wykres</button><button data-t="files">Pliki</button><button data-t="sys">System</button></nav>
<main>
<section id="live" class="on">
<div class="tabs" id="ptabs"></div>
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
<h3>STRONY ODCZYTOW (do 12 wartosci na strone)</h3>
<div class="tabs" id="etabs"></div>
<div id="sel"></div>
<p class="hint" id="selcnt"></p>
<h3>DODAJ DO STRONY</h3>
<label class="chk"><input type="checkbox" id="pall"> Wszystkie parametry (tysiace, nazwy tlumaczone automatycznie)</label>
<select id="mod"></select>
<input type="search" id="q" placeholder="Szukaj po nazwie...">
<div id="found"></div>
<h3>FORMAT LOGU CSV</h3>
<select id="csv"><option value="0">Excel PL (srednik, przecinek dziesietny)</option><option value="1">Standard (przecinek, kropka dziesietna)</option></select>
<h3>LOG CSV - CO ZAPISYWAC</h3>
<select id="logp"><option value="0">Strona 1</option><option value="1">Strona 2</option><option value="2">Strona 3</option><option value="3">Wszystkie strony (bez powtorzen)</option></select>
<p class="hint">Do logu zawsze dochodza serie wykresu.</p>
<p><button class="btn pri" id="save">Zapisz w urzadzeniu</button> <button class="btn" id="undo">Cofnij zmiany</button></p>
<div id="smsg" class="msg"></div>
</section>
<section id="chartset">
<h3>SERIE WYKRESU</h3>
<div id="chart"></div>
<p class="hint">Inna jednostka niz Seria 1 = prawa os wykresu.</p>
<h3>WYBIERZ PARAMETR</h3>
<label class="chk"><input type="checkbox" id="cpall"> Wszystkie parametry (tysiace, nazwy tlumaczone automatycznie)</label>
<select id="cmod"></select>
<input type="search" id="cq" placeholder="Szukaj po nazwie...">
<div id="cfound"></div>
<p><button class="btn pri" id="csave">Zapisz w urzadzeniu</button></p>
<div id="cmsg2" class="msg"></div>
</section>
<section id="sys">
<h3>URZADZENIE</h3>
<p id="about" class="hint"></p>
<h3>WI-FI</h3>
<p class="hint" id="wstate"></p>
<select id="wmode"><option value="0">Wlasna siec wyswietlacza</option><option value="1">Siec domowa (istniejaca)</option></select>
<div id="whome">
<p><button class="btn" id="wscan">Szukaj sieci</button></p>
<div id="wlist"></div>
<input type="search" id="wssid" placeholder="Nazwa sieci (SSID)">
<input type="search" id="wpass" placeholder="Haslo">
</div>
<p><button class="btn pri" id="wsave">Zapisz i polacz</button></p>
<p class="hint">Gdy sieci domowej nie ma (np. w aucie), po 20 s wlacza sie wlasna siec wyswietlacza. W sieci domowej strona jest pod adresem z ekranu urzadzenia albo http://diamel.local</p>
<div id="wmsg" class="msg"></div>
<h3>KATALOG PARAMETROW (KARTA SD /diamel)</h3>
<p class="hint">Pliki .csv z folderu docs/private/diamel_f31 (zaznacz wszystkie). Plik o tej samej nazwie zostanie zastapiony.</p>
<div id="clist"></div>
<input type="file" id="cfiles" accept=".csv" multiple>
<p><button class="btn pri" id="cupload">Wgraj pliki</button> <button class="btn del" id="cclear">Usun katalog</button></p>
<progress id="cprog" max="100" value="0" hidden></progress>
<div id="cmsg" class="msg"></div>
<h3>ZRZUT EKRANU I NAGRANIE</h3>
<p><button class="btn" id="shot">Zrzut ekranu</button> <a class="btn" id="shotdl" hidden>Zapisz PNG</a> <button class="btn" id="rec">Nagraj wideo</button> <a class="btn" id="recdl" hidden>Zapisz wideo</a></p>
<canvas id="scr" width="480" height="480" style="width:100%;max-width:480px;background:#000;border-radius:8px"></canvas>
<p class="hint">Obraz prosto z pamieci ekranu (bez aparatu). Nagranie: kolejne klatki tak szybko, jak pozwala Wi-Fi (ok. 2-4 na sekunde), zapis WebM - na GIF zamienisz go np. w ezgif.com.</p>
<h3>KOPIA USTAWIEN</h3>
<p class="hint">Plik JSON ze wszystkimi ustawieniami (strony Odczytow, wykres, usypianie, Wi-Fi razem z haslami). Po wgraniu urzadzenie uruchomi sie ponownie.</p>
<p><a class="btn" href="/api/backup">Pobierz kopie</a></p>
<input type="file" id="rfile" accept=".json">
<p><button class="btn" id="rupload">Wgraj kopie</button></p>
<div id="rmsg" class="msg"></div>
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
 if(t=='live')live();if(t=='files')files();if(t=='set'||t=='chartset'){if(!E)editStart();else renderEdit()}if(t=='sys'){catalog();wifiLoad()}}
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>show(b.dataset.t));

// --- Na zywo ---
function modName(m){const x=M.find(a=>a[0]==m);return x?x[1]:''}
// Strony Odczytow (1-3): zakladki nad kartami i w edytorze
let lp=0,ep=0;
function tabs(id,cur,fn){const t=$('#'+id);t.innerHTML='';
 [0,1,2].forEach(i=>{const b=el('button','btn'+(i==cur?' on':''),'Strona '+(i+1));b.onclick=()=>fn(i);t.append(b)})}
function buildCards(){tabs('ptabs',lp,i=>{lp=i;S.live=S.pages[lp];cardKey='';live()});const c=$('#cards');c.innerHTML='';
 if(!S.live.length)c.append(el('p','hint','Strona '+(lp+1)+' jest pusta - wartosci dodasz w zakladce Ustawienia.'));
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
function editStart(){E={pages:S.pages.map(p=>[...p]),chart:[...S.chart],csv:S.csv,log:S.log_page};E.live=E.pages[ep];
 $('#logp').value=E.log;$('#smsg').textContent='';renderEdit()}
function renderEdit(){const s=$('#sel');s.innerHTML='';tabs('etabs',ep,i=>{ep=i;E.live=E.pages[ep];renderEdit()});
 E.live.forEach((p,k)=>{const r=el('div','row');r.append(el('span','n',(k+1)+'. '+p[1]),el('span','s',modName(p[4])));
  const up=el('button','btn','↑'),dn=el('button','btn','↓'),x=el('button','btn del','✕');
  up.disabled=k==0;dn.disabled=k==E.live.length-1;
  up.onclick=()=>{[E.live[k-1],E.live[k]]=[E.live[k],E.live[k-1]];renderEdit()};
  dn.onclick=()=>{[E.live[k+1],E.live[k]]=[E.live[k],E.live[k+1]];renderEdit()};
  x.onclick=()=>{E.live.splice(k,1);renderEdit()};
  r.append(up,dn,x);s.append(r)});
 $('#selcnt').textContent='Strona '+(ep+1)+': '+E.live.length+' / '+S.max+(E.live.length?'':' - pusta');
 const c=$('#chart');c.innerHTML='';
 E.chart.forEach((p,k)=>{const r=el('div','row');r.append(el('span','n','Seria '+(k+1)+': '+(p?p[1]:'(brak)')));
  if(p){const x=el('button','btn del','✕');x.onclick=()=>{E.chart[k]=null;renderEdit()};r.append(x)}
  c.append(r)});
 $('#csv').value=E.csv;search('',0)}
// Wyszukiwanie po stronie urzadzenia (katalog ma tysiace parametrow), po 50 wynikow
// pr='' - zakladka Odczyty (dodaje do wybranej strony), pr='c' - zakladka Wykres (serie S1-S3)
let sTimer=null;
async function search(pr,off){const f=$('#'+pr+'found');if(!off)f.innerHTML='';
 const q=$('#'+pr+'q').value.trim(),m=$('#'+pr+'mod').value;
 if(!q&&m==-1){f.append(el('p','hint','Wybierz modul albo wpisz nazwe.'));return}
 try{const d=await j('/api/search?m='+m+'&off='+off+'&all='+($('#'+pr+'pall').checked?1:0)+'&q='+encodeURIComponent(q));
  const more=f.querySelector('.more');if(more)more.remove();
  d.items.forEach(p=>{const r=el('div','row');
   r.append(el('span','n',p[1]+(p[2]?' ['+p[2]+']':'')),el('span','s',m==-1?modName(p[4]):''));
   if(pr==''){const a=el('button','btn','+ Strona '+(ep+1));a.disabled=E.live.length>=S.max||E.live.some(x=>x[0]==p[0]);
    a.onclick=()=>{E.live.push(p);renderEdit();search('',0)};r.append(a)}
   else [0,1,2].forEach(k=>{const b=el('button','btn','S'+(k+1));b.title='Seria '+(k+1)+' wykresu';
    b.onclick=()=>{E.chart[k]=p;renderEdit()};r.append(b)});
   f.append(r)});
  if(!d.total)f.append(el('p','hint','Nic nie znaleziono.'));
  const shown=off+d.items.length;
  if(shown<d.total){const b=el('button','btn more','Pokaz wiecej ('+shown+' z '+d.total+')');b.onclick=()=>search(pr,shown);f.append(b)}}
 catch(e){f.append(el('p','hint','Blad: '+e.message))}}
['','c'].forEach(pr=>{
 $('#'+pr+'q').oninput=()=>{clearTimeout(sTimer);sTimer=setTimeout(()=>search(pr,0),300)};
 $('#'+pr+'mod').onchange=()=>search(pr,0);
 $('#'+pr+'pall').onchange=()=>search(pr,0)});
function fillModules(){['#mod','#cmod'].forEach(id=>{const s=$(id);s.innerHTML='';
 const o=el('option',null,'Wszystkie moduly');o.value=-1;s.append(o);
 M.forEach(m=>{const op=el('option',null,m[1]+' ('+m[2]+')');op.value=m[0];s.append(op)})})}
$('#csv').onchange=()=>{E.csv=+$('#csv').value};
$('#logp').onchange=()=>{E.log=+$('#logp').value};
$('#undo').onclick=editStart;
$('#csave').onclick=()=>save($('#cmsg2'));
$('#save').onclick=()=>save($('#smsg'));
// Jeden zapis dla obu zakladek (strony, wykres, log)
async function save(m){
 try{await post('/api/settings',{p0:E.pages[0].map(p=>p[0]).join(','),p1:E.pages[1].map(p=>p[0]).join(','),p2:E.pages[2].map(p=>p[0]).join(','),chart:E.chart.map(p=>p?p[0]:-1).join(','),csv:E.csv,log:E.log});
  S=await j('/api/settings');S.live=S.pages[lp];cardKey='';m.className='msg';m.textContent='Zapisano.'}
 catch(e){m.className='msg err';m.textContent='Blad: '+e.message}}

// --- Katalog parametrow na karcie SD ---
async function catalog(){const l=$('#clist');
 try{const d=await j('/api/catalog');l.innerHTML='';
  if(!d.sd){l.append(el('p','hint','Brak karty SD.'));return}
  if(!d.files.length)l.append(el('p','hint','Katalog pusty.'));
  d.files.forEach(f=>{const r=el('div','row');r.append(el('span','n',f[0]),el('span','s',size(f[1])));
   const x=el('button','btn del','Usun');x.onclick=async()=>{if(!confirm('Usunac '+f[0]+'?'))return;
    try{await post('/api/catalog_delete',{f:f[0]})}catch(e){alert(e.message)}catalog()};
   r.append(x);l.append(r)})}
 catch(e){l.textContent='Blad: '+e.message}}
$('#cupload').onclick=()=>{const fs=$('#cfiles').files,m=$('#cmsg'),pr=$('#cprog');
 if(!fs.length){m.className='msg err';m.textContent='Wybierz pliki .csv';return}
 const fd=new FormData();for(const f of fs)fd.append('f',f,f.name);
 const x=new XMLHttpRequest();x.open('POST','/api/catalog');
 x.upload.onprogress=e=>{if(e.lengthComputable)pr.value=e.loaded*100/e.total};
 x.onload=()=>{m.className=x.status==200?'msg':'msg err';m.style.whiteSpace='pre-line';
  m.textContent=x.responseText+(x.status==200?'\nUruchom urzadzenie ponownie (Info > Uruchom ponownie), aby wczytac katalog.':'');
  $('#cupload').disabled=false;catalog()};
 x.onerror=()=>{m.className='msg err';m.textContent='Przerwano polaczenie.';$('#cupload').disabled=false};
 pr.hidden=false;pr.value=0;m.textContent='';$('#cupload').disabled=true;x.send(fd)};
$('#cclear').onclick=async()=>{if(!confirm('Usunac caly katalog parametrow z karty?'))return;
 try{await post('/api/catalog_clear',{})}catch(e){alert(e.message)}catalog()};

// --- Wi-Fi: wlasna siec / siec domowa ---
const WST=['wylaczone','laczenie z siecia domowa...','wlasna siec','polaczono z siecia domowa'];
async function wifiLoad(){try{const d=await j('/api/wifi');
 $('#wmode').value=d.mode;$('#wssid').value=d.ssid;$('#wpass').value='';
 $('#wpass').placeholder=d.has_pass?'Haslo (puste = bez zmian)':'Haslo';
 $('#wstate').textContent='Teraz: '+WST[d.state]+(d.ip?', adres '+d.ip:'')+'. Wlasna siec: '+d.own;
 $('#whome').hidden=d.mode!=1}catch(e){}}
$('#wmode').onchange=()=>{$('#whome').hidden=$('#wmode').value!=1};
async function wscan(){const l=$('#wlist');l.textContent='Szukanie...';
 try{const d=await j('/api/scan');if(d.scanning){setTimeout(wscan,1500);return}
  l.innerHTML='';if(!d.networks.length)l.append(el('p','hint','Nie znaleziono sieci.'));
  [...new Map(d.networks.map(n=>[n[0],n])).values()].sort((a,b)=>b[1]-a[1]).forEach(n=>{if(!n[0])return;
   const r=el('div','row');r.append(el('span','n',n[0]),el('span','s',n[1]+' dBm'+(n[2]?'':' (otwarta)')));
   r.onclick=()=>{$('#wssid').value=n[0];$('#wpass').focus()};r.style.cursor='pointer';l.append(r)})}
 catch(e){l.textContent='Blad: '+e.message}}
$('#wscan').onclick=wscan;
$('#wsave').onclick=async()=>{const m=$('#wmsg'),mode=$('#wmode').value;
 try{await post('/api/wifi',{mode:mode,ssid:$('#wssid').value.trim(),pass:$('#wpass').value});
  m.className='msg';m.textContent=mode==1?'Zapisano. Wi-Fi uruchamia sie ponownie - polacz komputer z siecia '+$('#wssid').value+' i otworz http://diamel.local albo adres z ekranu urzadzenia.':'Zapisano. Wlacza sie wlasna siec wyswietlacza - polacz sie z nia ponownie.'}
 catch(e){m.className='msg err';m.textContent='Blad: '+e.message}};

// --- Zrzut ekranu i nagranie (klatki z pamieci ekranu urzadzenia) ---
const cv=$('#scr');let recOn=false,recorder=null;
function stamp(){return new Date().toISOString().slice(0,19).replace(/[-:T]/g,'')}
async function grab(){const r=await fetch('/api/screen.bmp?'+Date.now());if(!r.ok)throw new Error(r.status);
 const im=await createImageBitmap(await r.blob());cv.getContext('2d').drawImage(im,0,0)}
$('#shot').onclick=async()=>{try{await grab();const a=$('#shotdl');a.href=cv.toDataURL('image/png');
 a.download='diamel_'+stamp()+'.png';a.hidden=false}catch(e){alert('Blad: '+e.message)}};
$('#rec').onclick=async()=>{const b=$('#rec');
 if(recOn){recOn=false;return}
 if(!window.MediaRecorder){alert('Przegladarka nie nagrywa wideo (uzyj Chrome/Edge/Firefox)');return}
 const chunks=[];recorder=new MediaRecorder(cv.captureStream(10),{mimeType:'video/webm'});
 recorder.ondataavailable=e=>{if(e.data.size)chunks.push(e.data)};
 recorder.onstop=()=>{const a=$('#recdl');a.href=URL.createObjectURL(new Blob(chunks,{type:'video/webm'}));
  a.download='diamel_'+stamp()+'.webm';a.hidden=false;b.textContent='Nagraj wideo'};
 recOn=true;b.textContent='Zatrzymaj nagrywanie';await grab().catch(()=>{});recorder.start();
 while(recOn){try{await grab()}catch(e){await new Promise(r=>setTimeout(r,300))}}
 recorder.stop()};

// --- Kopia ustawien ---
$('#rupload').onclick=async()=>{const f=$('#rfile').files[0],m=$('#rmsg');
 if(!f){m.className='msg err';m.textContent='Wybierz plik .json';return}
 if(!confirm('Przywrocic ustawienia z '+f.name+'? Urzadzenie uruchomi sie ponownie.'))return;
 const fd=new FormData();fd.append('f',f,f.name);
 try{const r=await fetch('/api/restore',{method:'POST',body:fd});const t=await r.text();
  if(!r.ok)throw new Error(t);m.className='msg';m.textContent='Przywrocono ('+t+'). Urzadzenie uruchamia sie ponownie - wlacz Wi-Fi i odswiez strone.'}
 catch(e){m.className='msg err';m.textContent='Blad: '+e.message}};

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
 const p=await j('/api/params');M=p.modules;S=await j('/api/settings');S.live=S.pages[lp];
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
// --- Zrzut ekranu: obraz prosto z pamięci ekranu jako BMP (RGB565, bez przeliczania) ---
// Strona WWW robi z niego PNG albo nagrywa kolejne klatki do wideo.
#define BMP_HEADER 66 // nagłówek pliku 14 + nagłówek obrazu 40 + maski kolorów 12

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

static void handle_screen(AsyncWebServerRequest *req) {
    const uint16_t *fb = display_framebuffer();
    if (!fb) return req->send(503, "text/plain", "Brak obrazu");
    static uint8_t hdr[BMP_HEADER];
    const size_t row = SCREEN_W * 2; // 960 B - wielokrotność 4, bez dopełnienia
    const size_t total = BMP_HEADER + row * SCREEN_H;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(hdr + 2, total);
    put32(hdr + 10, BMP_HEADER);
    put32(hdr + 14, 40);
    put32(hdr + 18, SCREEN_W);
    put32(hdr + 22, SCREEN_H); // dodatnia wysokość = wiersze od dołu
    hdr[26] = 1;               // płaszczyzny
    hdr[28] = 16;              // bity na piksel
    put32(hdr + 30, 3);        // BI_BITFIELDS
    put32(hdr + 34, row * SCREEN_H);
    put32(hdr + 54, 0xF800);   // maski RGB565
    put32(hdr + 58, 0x07E0);
    put32(hdr + 62, 0x001F);
    AsyncWebServerResponse *res = req->beginResponse(
        "image/bmp", total, [fb, row, total](uint8_t *buf, size_t max, size_t index) -> size_t {
            size_t n = 0;
            while (n < max && index + n < total) {
                size_t pos = index + n;
                if (pos < BMP_HEADER) {
                    buf[n++] = hdr[pos];
                    continue;
                }
                size_t p = pos - BMP_HEADER, y = p / row, off = p % row;
                size_t chunk = std::min(row - off, max - n);
                memcpy(buf + n, (const uint8_t *)(fb + (SCREEN_H - 1 - y) * SCREEN_W) + off, chunk);
                n += chunk;
            }
            return n;
        });
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

// --- Kopia ustawień: pobranie i wgranie pliku JSON ---
#define RESTORE_MAX 16384
static char *restore_buf;      // w PSRAM, na czas wgrywania
static size_t restore_len;
static bool restore_overflow;
static Settings restore_settings;
static volatile bool restore_pending = false;

static void handle_backup(AsyncWebServerRequest *req) {
    size_t n;
    char *json = backup_to_json(&n);
    if (!json) return req->send(500, "text/plain", "Brak pamieci");
    AsyncWebServerResponse *res = req->beginResponse(200, "application/json", (const uint8_t *)json, n);
    res->addHeader("Content-Disposition", "attachment; filename=diamel_ustawienia.json");
    req->send(res);
    free(json);
}

static void handle_restore_upload(AsyncWebServerRequest *, const String &, size_t index, uint8_t *data, size_t len,
                                  bool) {
    if (index == 0) {
        if (!restore_buf) restore_buf = (char *)heap_caps_malloc(RESTORE_MAX, MALLOC_CAP_SPIRAM);
        restore_len = 0;
        restore_overflow = false;
    }
    if (!restore_buf || restore_len + len > RESTORE_MAX) {
        restore_overflow = true;
        return;
    }
    memcpy(restore_buf + restore_len, data, len);
    restore_len += len;
}

static void handle_restore_done(AsyncWebServerRequest *req) {
    if (restore_overflow || !restore_len) return req->send(400, "text/plain", "Plik pusty albo za duzy");
    char err[80];
    int skipped = 0;
    if (!backup_from_json(restore_buf, restore_len, restore_settings, skipped, err, sizeof(err))) {
        return req->send(400, "text/plain", err);
    }
    restore_pending = true; // zadanie UI zapisze ustawienia i uruchomi urządzenie ponownie
    char msg[80];
    snprintf(msg, sizeof(msg), "OK%s", skipped ? " (pominieto parametrow spoza katalogu)" : "");
    req->send(200, "text/plain", msg);
}

// --- Wi-Fi: tryb, sieć domowa, wyszukiwanie sieci ---
static void handle_wifi_get(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["mode"] = g_settings.wifi_mode;
    doc["ssid"] = g_settings.home_ssid;
    doc["has_pass"] = g_settings.home_pass[0] != 0; // hasła nie odsyłamy
    doc["state"] = (int)wstate;
    doc["ip"] = ip_text;
    doc["own"] = wifi_ssid();
    send_json(req, doc);
}

static void handle_wifi_post(AsyncWebServerRequest *req) {
    WebWifi w = {};
    long mode = form_value(req, "mode").toInt();
    String ssid = form_value(req, "ssid"), pass = form_value(req, "pass");
    if (mode < 0 || mode >= WIFI_MODE_COUNT) return req->send(400, "text/plain", "Zly tryb");
    if (mode == WIFI_MODE_HOME && !ssid.length()) return req->send(400, "text/plain", "Podaj nazwe sieci");
    if (ssid.length() > 32 || pass.length() > 64) return req->send(400, "text/plain", "Za dluga nazwa lub haslo");
    w.mode = (uint8_t)mode;
    strlcpy(w.ssid, ssid.c_str(), sizeof(w.ssid));
    strlcpy(w.pass, pass.c_str(), sizeof(w.pass));
    w.keep_pass = !pass.length() && ssid == g_settings.home_ssid; // puste hasło = bez zmian (ta sama sieć)
    portENTER_CRITICAL(&mux);
    pending_wifi = w;
    pending_wifi_set = true;
    portEXIT_CRITICAL(&mux);
    req->send(200, "text/plain", "OK");
}

// Wyszukiwanie w tle: pierwsze zapytanie startuje, kolejne odbierają wynik
static void handle_scan(AsyncWebServerRequest *req) {
    JsonDocument doc;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true);
        n = WIFI_SCAN_RUNNING;
    }
    doc["scanning"] = n == WIFI_SCAN_RUNNING;
    JsonArray list = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonArray a = list.add<JsonArray>();
        a.add(WiFi.SSID(i));
        a.add(WiFi.RSSI(i));
        a.add(WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    if (n >= 0) WiFi.scanDelete();
    send_json(req, doc);
}

static void handle_time(AsyncWebServerRequest *req) {
    if (!g_settings.time_from_browser) return req->send(200, "text/plain", "Pominieto (wylaczone w ustawieniach)");
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
    bool all = req->hasParam("all") && req->getParam("all")->value() == "1";
    int total = params_search(module, q.c_str(), offset, found, 50, all);
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
    while (*s && n < PAGE_MAX + CHART_SERIES_MAX) {
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
    doc["max"] = PAGE_MAX;
    JsonArray pages = doc["pages"].to<JsonArray>();
    for (int p = 0; p < LIVE_PAGES; p++) {
        JsonArray page = pages.add<JsonArray>();
        for (int i = 0; i < g_settings.page_count[p]; i++) add_param(page, g_settings.page_ids[p][i]);
    }
    doc["log_page"] = g_settings.log_page;
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
    // Strony p0, p1, p2: do 12 wartości, bez powtórzeń w obrębie strony
    for (int p = 0; p < LIVE_PAGES; p++) {
        char name[4] = {'p', (char)('0' + p), 0};
        int n = parse_ids(form_value(req, name), w.page_ids[p], PAGE_MAX, false);
        if (n < 0) return req->send(400, "text/plain", "Bledna lista odczytow");
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < i; k++) {
                if (w.page_ids[p][k] == w.page_ids[p][i]) return req->send(400, "text/plain", "Powtorzona wartosc");
            }
        }
        w.page_count[p] = n;
    }
    long log_page = form_value(req, "log").toInt();
    if (log_page < 0 || log_page > LOG_ALL_PAGES) return req->send(400, "text/plain", "Bledna strona logu");
    w.log_page = (uint8_t)log_page;
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
static String cat_report; // wynik dla każdego pliku: "nazwa: OK" albo powód błędu

static void cat_fail(const String &name, const char *why) {
    cat_error = true;
    cat_report += name + ": " + why + "\n";
    Serial.printf("Katalog: %s - %s\n", name.c_str(), why);
}

static void handle_catalog_upload(AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data,
                                  size_t len, bool final) {
    if (req != cat_req) { // pierwszy plik nowego zapytania
        cat_req = req;
        cat_error = false;
        cat_count = 0;
        cat_report = "";
    }
    if (index == 0) {
        if (cat_file) cat_file.close();
        if (!logger_sd_ok()) return cat_fail(filename, "brak karty SD");
        if (!filename.endsWith(".csv")) return cat_fail(filename, "to nie jest plik .csv");
        if (!logger_valid_name(filename.c_str())) {
            return cat_fail(filename, "niedozwolona nazwa (litery, cyfry, _ - . i do 23 znakow; bez spacji i nawiasow)");
        }
        if (!SD.exists(CATALOG_DIR)) SD.mkdir(CATALOG_DIR);
        SD.remove(CATALOG_TMP); // pozostałość po przerwanym wgrywaniu
        cat_target = String(CATALOG_DIR) + "/" + filename;
        cat_file = SD.open(CATALOG_TMP, FILE_WRITE);
        if (!cat_file) return cat_fail(filename, "nie mozna utworzyc pliku na karcie");
    }
    if (!cat_file) return;
    if (len && cat_file.write(data, len) != len) {
        cat_file.close();
        SD.remove(CATALOG_TMP);
        return cat_fail(filename, "blad zapisu na karte (pelna karta?)");
    }
    if (final) {
        cat_file.close();
        if (SD.exists(cat_target) && !SD.remove(cat_target)) {
            SD.remove(CATALOG_TMP);
            return cat_fail(filename, "nie mozna zastapic starego pliku");
        }
        if (!SD.rename(CATALOG_TMP, cat_target)) return cat_fail(filename, "nie mozna zmienic nazwy pliku tymczasowego");
        cat_count++;
        cat_report += filename + ": OK (" + String((unsigned)(index + len)) + " B)\n";
        Serial.printf("Katalog: %s, %u B\n", cat_target.c_str(), (unsigned)(index + len));
    }
}

static void handle_catalog_done(AsyncWebServerRequest *req) {
    if (cat_file) cat_file.close();
    req->send(cat_error ? 500 : 200, "text/plain", cat_report.length() ? cat_report : String("Brak plikow"));
    cat_req = nullptr;
}

// Usunięcie jednego pliku katalogu (np. przed wgraniem nowszej wersji)
static void handle_catalog_delete(AsyncWebServerRequest *req) {
    String name = form_value(req, "f");
    if (!logger_sd_ok() || !logger_valid_name(name.c_str())) return req->send(400, "text/plain", "Zla nazwa");
    if (SD.remove(String(CATALOG_DIR) + "/" + name)) req->send(200, "text/plain", "OK");
    else req->send(404, "text/plain", "Nie mozna usunac");
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
    server.on("/api/wifi", HTTP_GET, handle_wifi_get);
    server.on("/api/wifi", HTTP_POST, handle_wifi_post);
    server.on("/api/scan", HTTP_GET, handle_scan);
    server.on("/api/backup", HTTP_GET, handle_backup);
    server.on("/api/screen.bmp", HTTP_GET, handle_screen);
    server.on("/api/restore", HTTP_POST, handle_restore_done, handle_restore_upload);
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
    server.on("/api/catalog_delete", HTTP_POST, handle_catalog_delete);
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

static void ensure_server() {
    if (!server_started) {
        setup_routes();
        server.begin();
        server_started = true;
    }
}

// Własna sieć; tryb AP+STA, żeby dało się wyszukać sieci domowe ze strony WWW
static bool start_own_network() {
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(wifi_ssid(), g_settings.wifi_pass, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
        Serial.println("Wi-Fi: BLAD uruchomienia punktu dostepowego");
        WiFi.mode(WIFI_OFF);
        return false;
    }
    strlcpy(ip_text, WiFi.softAPIP().toString().c_str(), sizeof(ip_text));
    ensure_server();
    wstate = WSTATE_OWN;
    active = true;
    Serial.printf("Wi-Fi: wlasna siec %s, adres http://%s, wolny RAM wewn.: %u KB\n", ssid, ip_text,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

bool wifi_start() {
    if (active) return true;
    if (g_settings.wifi_mode == WIFI_MODE_HOME && g_settings.home_ssid[0]) {
        // Łączenie trwa kilka sekund - dokończy je wifi_tick() (bez blokowania ekranu)
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(MDNS_NAME);
        WiFi.setAutoReconnect(true);
        WiFi.begin(g_settings.home_ssid, g_settings.home_pass);
        ensure_server();
        ip_text[0] = 0;
        wstate = WSTATE_CONNECTING;
        connect_ms = millis();
        active = true;
        Serial.printf("Wi-Fi: laczenie z siecia %s...\n", g_settings.home_ssid);
        return true;
    }
    return start_own_network();
}

void wifi_stop() {
    if (!active) return;
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    active = false;
    wstate = WSTATE_OFF;
    ip_text[0] = 0;
    Serial.println("Wi-Fi: wylaczone");
}

bool wifi_active() { return active; }

WifiState wifi_state() { return wstate; }

uint8_t wifi_clients() { return wstate == WSTATE_OWN ? WiFi.softAPgetStationNum() : 0; }

void wifi_restart() {
    if (!active) return;
    wifi_stop();
    wifi_start();
}

// Łączenie z siecią domową: sukces -> adres IP i diamel.local; brak sieci -> własna sieć (dostęp zawsze jest)
static void connect_tick() {
    if (wstate == WSTATE_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            strlcpy(ip_text, WiFi.localIP().toString().c_str(), sizeof(ip_text));
            if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
            wstate = WSTATE_HOME;
            static char msg[64];
            snprintf(msg, sizeof(msg), "Wi-Fi: polaczono (%s)", ip_text);
            message = msg;
            Serial.printf("Wi-Fi: polaczono z %s, adres http://%s, http://%s.local\n", g_settings.home_ssid, ip_text,
                          MDNS_NAME);
        } else if (millis() - connect_ms > HOME_CONNECT_TIMEOUT_MS) {
            Serial.println("Wi-Fi: brak sieci domowej - wlaczam wlasna siec");
            WiFi.disconnect(true);
            start_own_network();
            message = "Brak sieci domowej - wlaczono wlasna siec";
        }
    }
    // Zmiana sieci ze strony WWW: restart po wysłaniu odpowiedzi
    if (wifi_reconfig_ms && millis() - wifi_reconfig_ms > 1000) {
        wifi_reconfig_ms = 0;
        wifi_restart();
    }
}

const char *wifi_ip() { return ip_text; }

int wifi_ota_progress() { return ota_progress; }

bool wifi_apply_web_settings() {
    if (!pending_set) return false;
    portENTER_CRITICAL(&mux);
    WebSettings w = pending;
    pending_set = false;
    portEXIT_CRITICAL(&mux);

    memcpy(g_settings.page_count, w.page_count, sizeof(w.page_count));
    memcpy(g_settings.page_ids, w.page_ids, sizeof(w.page_ids));
    g_settings.log_page = w.log_page;
    memcpy(g_settings.chart_series, w.chart_series, sizeof(w.chart_series));
    g_settings.csv_format = w.csv_format;
    settings_mark_dirty();
    return true;
}

void wifi_tick() {
    // Sieć domowa zapisana na stronie WWW: do ustawień, potem restart Wi-Fi (po wysłaniu odpowiedzi)
    if (pending_wifi_set) {
        portENTER_CRITICAL(&mux);
        WebWifi w = pending_wifi;
        pending_wifi_set = false;
        portEXIT_CRITICAL(&mux);
        g_settings.wifi_mode = w.mode;
        strlcpy(g_settings.home_ssid, w.ssid, sizeof(g_settings.home_ssid));
        if (!w.keep_pass) strlcpy(g_settings.home_pass, w.pass, sizeof(g_settings.home_pass));
        settings_mark_dirty();
        wifi_reconfig_ms = millis() | 1;
    }
    connect_tick();

    // Kopia ustawień wgrana przez WWW: zapis i restart (ten sam tryb co po aktualizacji)
    if (restore_pending) {
        restore_pending = false;
        g_settings = restore_settings;
        settings_mark_dirty();
        message = "Przywrocono ustawienia - restart";
        restart_pending = true;
    }

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
