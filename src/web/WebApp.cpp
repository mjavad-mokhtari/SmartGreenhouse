#include "web/WebApp.h"
#include "sync.h"

WebApp::WebApp(RtcManager& rtc, WifiManager& wifi
#ifdef MODULE_IRRIGATION
  , IrrigationService& irrigation
#endif
#ifdef MODULE_LIGHTING
  , LightingService& lighting
#endif
) : rtc(rtc), wifi(wifi)
#ifdef MODULE_IRRIGATION
  , irrigation(irrigation)
#endif
#ifdef MODULE_LIGHTING
  , lighting(lighting)
#endif
{}

void WebApp::sendJson(AsyncWebServerRequest* request, const String& body, int code) {
  AsyncWebServerResponse* r = request->beginResponse(code, "application/json", body);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
}

String WebApp::buildStatusJson() {
  DynamicJsonDocument doc(1024);
  DateTime now = rtc.now();
  char timeBuf[20];
  snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  doc["time"] = timeBuf;
  doc["uptime"] = millis();
  doc["freeHeap"] = ESP.getFreeHeap();

  JsonObject w = doc.createNestedObject("wifi");
  w["connected"] = wifi.connected();
  w["ip"] = wifi.ip();
  w["network"] = wifi.network();
  w["apMode"] = wifi.isAccessPoint();

  JsonObject r = doc.createNestedObject("rtc");
  r["available"] = rtc.isAvailable();
  r["valid"] = (now.year() >= 2020);

  JsonObject h = doc.createNestedObject("health");
  h["wifiDisconnected"] = !wifi.connected();
  h["rtcValid"] = (now.year() >= 2020);
  h["freeHeapKB"] = ESP.getFreeHeap() / 1024;
  h["uptimeMin"] = millis() / 60000;

  // Events
  JsonArray ev = h.createNestedArray("events");
  for (size_t i = 0; i < _syncCount && i < 5; i++) {
    size_t idx = (_syncHead + _syncCount - 1 - i) % SYNC_RING_SIZE;
    JsonObject e = ev.createNestedObject();
    e["type"] = _syncEvents[idx].type;
    e["state"] = _syncEvents[idx].state;
    e["ts"] = _syncEvents[idx].ts;
  }

#ifdef MODULE_IRRIGATION
  irrigation.appendStatus(doc.as<JsonObject>());
#endif
#ifdef MODULE_LIGHTING
  if (!doc.containsKey("lighting"))
    lighting.appendStatus(doc.as<JsonObject>());
#endif

  JsonObject sc = doc.createNestedObject("sync");
  sc["url"] = getServerUrl();
  sc["enabled"] = getServerEnabled();
  sc["online"] = serverOnlineFlag();
  sc["pending"] = pendingCount();
  sc["deviceId"] = getDeviceId();
  sc["device_id"] = getDeviceId();

  String out;
  serializeJson(doc, out);
  return out;
}

// =========== Handler implementations (unchanged core logic) ===========

void WebApp::handleStatus(AsyncWebServerRequest* request) {
  sendJson(request, buildStatusJson());
}

void WebApp::handleHealth(AsyncWebServerRequest* request) {
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

void WebApp::handleLogs(AsyncWebServerRequest* request) {
  String json = "{\"count\":" + String(_syncCount) + ",\"events\":" + eventsJson() + "}";
  sendJson(request, json);
}

void WebApp::handleDashboard(AsyncWebServerRequest* request) {
  request->send(200, "text/html", buildFullDashboard());
}

#ifdef MODULE_IRRIGATION
void WebApp::handleZoneOn(AsyncWebServerRequest* request) {
  String idStr = request->hasParam("id", true) ? request->getParam("id", true)->value() : request->arg("id");
  String durStr = request->hasParam("dur", true) ? request->getParam("dur", true)->value() : request->arg("dur");
  uint8_t id = idStr.toInt();
  uint16_t dur = durStr.length() > 0 ? durStr.toInt() : 15;
  if (dur == 0) dur = 15;
  int idx = irrigation.findZone(id);
  if (idx >= 0 && irrigation.start(idx, dur, "HTTP manual start"))
    sendJson(request, "{\"ok\":true}");
  else
    sendJson(request, "{\"ok\":false}", 400);
}

void WebApp::handleZoneOff(AsyncWebServerRequest* request) {
  String idStr = request->hasParam("id", true) ? request->getParam("id", true)->value() : request->arg("id");
  uint8_t id = idStr.toInt();
  int idx = irrigation.findZone(id);
  if (idx >= 0 && irrigation.stop(idx, "HTTP manual stop"))
    sendJson(request, "{\"ok\":true}");
  else
    sendJson(request, "{\"ok\":false}", 400);
}

void WebApp::handleZoneAdd(AsyncWebServerRequest* request) {
  String idStr = request->hasParam("id", true) ? request->getParam("id", true)->value() : request->arg("id");
  String pinStr = request->hasParam("pin", true) ? request->getParam("pin", true)->value() : request->arg("pin");
  if (irrigation.addZone(idStr.toInt(), pinStr.toInt()))
    sendJson(request, "{\"ok\":true}");
  else
    sendJson(request, "{\"ok\":false}", 400);
}

void WebApp::handleZoneSchedule(AsyncWebServerRequest* request) {
  // Read params from URL query string (most reliable for AsyncWebServer POST)
  String idStr = request->arg("id");
  String hourStr = request->arg("hour");
  String minStr = request->arg("minute");
  if (minStr.length() == 0) minStr = request->arg("min");
  String durStr = request->arg("dur");

  Serial.printf("SCHEDULE: id=%s h=%s m=%s dur=%s\n",
    idStr.c_str(), hourStr.c_str(), minStr.c_str(), durStr.c_str());

  int idVal = idStr.length() > 0 ? idStr.toInt() : -1;
  int hVal = hourStr.length() > 0 ? hourStr.toInt() : 0;
  int mVal = minStr.length() > 0 ? minStr.toInt() : 0;
  int dVal = durStr.length() > 0 ? durStr.toInt() : 0;
  if (dVal == 0) dVal = 15;

  int idx = irrigation.findZone(idVal);
  if (idx >= 0 && irrigation.setSchedule(idx, (uint8_t)hVal, (uint8_t)mVal, (uint16_t)dVal)) {
    queueEvent("schedule", "set");
    Serial.printf("SCHEDULE OK: zone %d set to %02d:%02d for %d min\n", idVal, hVal, mVal, dVal);
    sendJson(request, "{\"ok\":true}");
  } else {
    Serial.printf("SCHEDULE FAIL: idx=%d h=%d m=%d d=%d zoneN=%d\n", idx, hVal, mVal, dVal, irrigation.zoneCount());
    sendJson(request, "{\"ok\":false}", 400);
  }
}

void WebApp::handlePumpOn(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  irrigation.setPump(true, "HTTP manual start");
  sendJson(request, "{\"pump\":\"on\"}");
}

void WebApp::handlePumpOff(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  for (uint8_t i = 0; i < irrigation.zoneCount(); i++) irrigation.stop(i, "Pump stop");
  irrigation.setPump(false, "HTTP manual stop");
  sendJson(request, "{\"pump\":\"off\"}");
}

void WebApp::handleAllOff(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  for (uint8_t i = 0; i < irrigation.zoneCount(); i++) irrigation.stop(i, "ESTOP");
  irrigation.setPump(false, "ESTOP");
  sendJson(request, "{\"ok\":true}");
}
#endif

#ifdef MODULE_LIGHTING
void WebApp::handleLightToggle(AsyncWebServerRequest* request) {
  String idStr = request->hasParam("id", true) ? request->getParam("id", true)->value() : request->arg("id");
  if (lighting.toggleChannel(idStr.toInt()))
    sendJson(request, "{\"ok\":true}");
  else
    sendJson(request, "{\"ok\":false}", 400);
}

void WebApp::handleLightAllOn(AsyncWebServerRequest* request)  { lighting.allOn();  sendJson(request, "{\"ok\":true}"); }
void WebApp::handleLightAllOff(AsyncWebServerRequest* request) { lighting.allOff(); sendJson(request, "{\"ok\":true}"); }
#endif

void WebApp::handleConfigTime(AsyncWebServerRequest* request) {
  String y = request->hasParam("year", true) ? request->getParam("year", true)->value() : request->arg("year");
  String mo = request->hasParam("month", true) ? request->getParam("month", true)->value() : request->arg("month");
  String d = request->hasParam("day", true) ? request->getParam("day", true)->value() : request->arg("day");
  String h = request->hasParam("hour", true) ? request->getParam("hour", true)->value() : request->arg("hour");
  String mi = request->hasParam("minute", true) ? request->getParam("minute", true)->value() : request->arg("minute");
  if (y.length() > 0) {
    rtc.set(DateTime(y.toInt(), mo.toInt(), d.toInt(),
      h.length() > 0 ? h.toInt() : 0, mi.length() > 0 ? mi.toInt() : 0, 0));
    sendJson(request, "{\"ok\":true}");
  } else {
    sendJson(request, "{\"ok\":false}", 400);
  }
}

void WebApp::handleConfigWifi(AsyncWebServerRequest* request) {
  String ssid = request->hasParam("ssid", true) ? request->getParam("ssid", true)->value() : request->arg("ssid");
  String pass = request->hasParam("password", true) ? request->getParam("password", true)->value() : request->arg("password");
  if (ssid.length() > 0) {
    wifi.save(ssid, pass);
    sendJson(request, "{\"ok\":true}");
    delay(1500);
    ESP.restart();
  } else {
    sendJson(request, "{\"ok\":false}", 400);
  }
}

void WebApp::handleConfigSync(AsyncWebServerRequest* request) {
  String url = request->hasParam("url", true) ? request->getParam("url", true)->value() : request->arg("url");
  String apiKey = request->hasParam("key", true) ? request->getParam("key", true)->value() : request->arg("key");
  String devId = request->hasParam("devid", true) ? request->getParam("devid", true)->value() : request->arg("devid");
  
  if (url.length() > 0) {
    setServerUrl(url);
    setServerEnabled(true);
    queueEvent("config", "sync-url-set");
  }
  if (apiKey.length() > 0) {
    setApiKey(apiKey);
    queueEvent("config", "api-key-set");
  }
  if (devId.length() > 0) {
    setDeviceId(devId);
    queueEvent("config", "device-id-set");
  }
  syncTriggerNow();
  sendJson(request, "{\"ok\":true}");
}

// =========== DASHBOARD : Mobile-first, DOM-patch refresh ===========

String WebApp::buildFullDashboard() {
  return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>خانه سبز هوشمند</title>
<style>
:root{--bg:#0f1115;--card:#1a1d22;--fg:#e2e6e9;--accent:#4ade80;--warn:#fbbf24;--red:#f87171;--muted:#6b7280;--border:#2a2d33;--radius:12px}
*,*::before,*::after{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,sans-serif;-webkit-tap-highlight-color:transparent}
main{max-width:640px;margin:auto;padding:16px}
/* Header */
.topbar{display:flex;justify-content:space-between;align-items:center;padding:8px 0 16px;border-bottom:1px solid var(--border);margin-bottom:12px}
.topbar h1{font:bold 1.3rem Georgia,serif;color:var(--accent);margin:0;letter-spacing:.5px}
.topbar .tm{font-size:.78rem;color:var(--muted)}
/* Tabs */
.tabs{display:flex;gap:4px;margin:12px 0;overflow-x:auto;scrollbar-width:none;-ms-overflow-style:none}
.tabs::-webkit-scrollbar{display:none}
.tab{flex-shrink:0;padding:8px 14px;border:1px solid var(--border);border-radius:20px;font-size:.82rem;cursor:pointer;background:var(--bg);color:var(--muted);transition:all .15s}
.tab.on{background:var(--accent);color:#000;border-color:var(--accent);font-weight:600}
/* Insight row */
.insights{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:10px;margin-bottom:14px}
.insight{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);padding:12px 14px;text-align:center}
.insight .n{font-size:1.5rem;font-weight:700;color:var(--accent);line-height:1.1}
.insight .n.danger{color:var(--red)}.insight .n.warn{color:var(--warn)}
.insight .l{font-size:.72rem;color:var(--muted);margin-top:3px;text-transform:uppercase;letter-spacing:.3px}
/* Cards */
.card{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);padding:14px;margin-bottom:10px}
.card .hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.card .ttl{font-size:.82rem;color:var(--muted);text-transform:uppercase;letter-spacing:.4px}
.card .st{font-size:.78rem;font-weight:600;border-radius:10px;padding:2px 10px}
.st-on{background:#166534;color:var(--accent)}.st-off{background:#27272a;color:var(--muted)}.st-run{background:#7c2d12;color:var(--warn)}
/* Buttons */
.bt{display:inline-flex;align-items:center;justify-content:center;gap:4px;padding:10px 18px;border:none;border-radius:8px;font-size:.84rem;font-weight:600;cursor:pointer;color:#fff;transition:opacity .15s;min-width:56px}
.bt:active{opacity:.75}
.bt-g{background:#166534}.bt-r{background:#991b1b}.bt-o{background:#1e3a5f}.bt-w{background:#92400e}
/* Form row */
.fr{display:flex;align-items:center;gap:8px;margin-top:8px;flex-wrap:wrap}
.fr input[type=time],.fr input[type=number]{background:var(--bg);border:1px solid var(--border);color:var(--fg);padding:8px 10px;border-radius:6px;font-size:.84rem;width:auto;min-width:0}
.fr input[type=number]{width:52px}
.fr input[type=time]{width:100px}
.fr select{background:var(--bg);border:1px solid var(--border);color:var(--fg);padding:8px;border-radius:6px;font-size:.84rem}
/* Log feed */
.logs{max-height:260px;overflow-y:auto;font-size:.75rem;font-family:monospace;line-height:1.6;background:#0a0b0e;border-radius:8px;padding:10px}
.logs .e{padding:3px 0;border-bottom:1px solid rgba(255,255,255,.03)}
.logs .e .tp{color:var(--accent);font-weight:600}
.logs .e .tm{color:var(--muted);margin-left:6px}
/* Settings */
.set{padding:14px;background:var(--card);border:1px solid var(--border);border-radius:var(--radius);margin-bottom:10px}
.set h3{font-size:.9rem;color:var(--muted);margin:0 0 10px}
/* Emergency button */
.estop{margin:18px 0;text-align:center}
.estop .bt{font-size:1rem;padding:14px 40px;border-radius:24px;background:var(--red);animation:pulse 2s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
.hidden{display:none!important}
/* Responsive */
@media(min-width:768px){main{max-width:900px}.insights{grid-template-columns:repeat(4,1fr)}}
</style>
</head>
<body>
<main>
<div class="topbar"><h1>🏠 خانه سبز هوشمند</h1><span class="tm" id="clock">--:--</span></div>

<!-- Insight bar -->
<div class="insights" id="insights"></div>

<!-- Tabs -->
<nav class="tabs" id="tabs">
  <button class="tab" data-pg="overview">Overview</button>
  <button class="tab on" data-pg="irrigation">Irrigation</button>
  <button class="tab" data-pg="lighting">Lighting</button>
  <button class="tab" data-pg="logs">Logs</button>
  <button class="tab" data-pg="settings">Settings</button>
</nav>

<!-- Pages -->
<div id="pg-overview" class="hidden"></div>
<div id="pg-irrigation"></div>
<div id="pg-lighting" class="hidden"></div>
<div id="pg-logs" class="hidden"></div>
<div id="pg-settings" class="hidden"></div>

<div class="estop"><button class="bt" onclick="api('e')">EMERGENCY STOP</button></div>
</main>

<script>
// --- State ---
let st = null; // last status JSON
let page = 'irrigation';

// --- Smart DOM patching ---
function $(id){return document.getElementById(id)}
function isFocusedInside(id){
  var ae=document.activeElement;
  if(!ae)return false;
  // Only block refresh when editing text/select — not for buttons
  var t=ae.tagName;
  if(t!='INPUT'&&t!='TEXTAREA'&&t!='SELECT')return false;
  var c=$(id);
  if(!c)return false;
  while(ae){if(ae===c)return true;ae=ae.parentElement}
  return false
}

function el(tag,cls,html){
  const e=document.createElement(tag);
  if(cls)e.className=cls;
  if(html!==undefined)e.innerHTML=html;
  return e
}

// --- API ---
function api(a,p){fetch('/api/'+a+(p||''),{method:'POST'}).then(refresh)}

// --- Render all ---
function refresh(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    st=s;
    renderClock(s);
    renderInsights(s);
    renderIrrigation(s);
    renderLighting(s);
  }).catch(()=>{});
  fetch('/api/logs').then(r=>r.json()).then(l=>renderLogs(l)).catch(()=>{});
}

// --- Clock ---
function renderClock(s){
  if(s.time)$('clock').textContent=s.time;
}

// --- Insights (always visible at top) ---
function renderInsights(s){
  const w=s.wifi||{}, rt=s.rtc||{}, irr=s.irrigation||{}, lt=s.lighting||{}, h=s.health||{};
  let activeZones=0; if(irr.zones)irr.zones.forEach(z=>{if(z.running)activeZones++});
  let lightsOn=0; if(lt.state)lt.state.forEach(c=>{if(c.state)lightsOn++});
  const soil=irr.soilPercent!=null?irr.soilPercent:'--';

  const items=[
    {v:w.connected?'Online':'Offline',l:'Wi-Fi',c:w.connected?'':'warn'},
    {v:irr.pumpOn?'ON':'OFF',l:'Pump',c:irr.pumpOn?'':'danger'},
    {v:activeZones,l:'Active Zones',c:activeZones?'warn':''},
    {v:soil+'%',l:'Soil Moisture',c:soil<30?'danger':''},
    {v:lightsOn,l:'Lights On',c:''},
    {v:h.freeHeapKB+' KB',l:'Free RAM',c:''},
    {v:rt.valid?'Valid':'Invalid',l:'RTC',c:rt.valid?'':'danger'},
    {v:h.uptimeMin+' min',l:'Uptime',c:''}
  ];

  let html='';
  items.forEach(it=>{
    html+='<div class="insight"><div class="n'+ (it.c?' '+it.c:'') +'">'+it.v+'</div><div class="l">'+it.l+'</div></div>';
  });
  $('insights').innerHTML=html;
}

// --- Irrigation page ---
function renderIrrigation(s){
  const irr=s.irrigation;
  if(isFocusedInside('pg-irrigation'))return;
  if(!irr){$('pg-irrigation').innerHTML='<div class="card"><span class="ttl">Irrigation inactive</span></div>';return;}

  let html='';

  // Pump card
  html+='<div class="card"><div class="hdr"><span class="ttl">Pump • GPIO'+irr.pumpGpio+'</span>';
  html+='<span class="st '+ (irr.pumpOn?'st-run':'st-off') +'">'+ (irr.pumpOn?'RUNNING':'IDLE') +'</span></div>';
  html+='<div class="fr">';
  html+='<button class="bt bt-g" onclick="api(\'pump/on\')">ON</button>';
  html+='<button class="bt bt-r" onclick="api(\'pump/off\')">OFF</button>';
  html+='</div></div>';

  // Soil
  html+='<div class="card"><span class="ttl">Soil Sensor</span>';
  html+='<div class="n'+(irr.soilPercent<30?' danger':'')+'">'+irr.soilPercent+'%</div>';
  html+='<span style="font-size:.75rem;color:var(--muted)">Raw: '+irr.soilRaw+'</span></div>';

  // Zones
  const zones=irr.zones||[];
  zones.forEach(z=>{
    const run=z.running;
    html+='<div class="card"><div class="hdr"><span class="ttl">Zone '+z.id+' • GPIO'+z.pin+'</span>';
    html+='<span class="st '+ (run?'st-run':'st-off') +'">'+ (run?'RUNNING ('+z.remainingSec+'s)':'IDLE') +'</span></div>';
    html+='<div style="font-size:.78rem;color:var(--muted);margin-bottom:6px">Next: '+(z.nextRun||'--')+' | Enabled: '+(z.enabled?'Yes':'No')+'</div>';
    html+='<div class="fr">';
    html+='<button class="bt bt-g" onclick="api(\'zone/on?id='+z.id+'&dur='+z.duration+'\')">ON</button>';
    html+='<button class="bt bt-r" onclick="api(\'zone/off?id='+z.id+'\')">OFF</button>';
    html+='</div>';
    html+='<div class="fr">';
    html+='<input type="time" id="sch-t'+z.id+'" value="'+pad(z.hour)+':'+pad(z.minute)+'">';
    html+='<input type="number" id="sch-d'+z.id+'" value="'+(z.duration||15)+'" min="1" max="120" style="width:45px">';
    html+='<span style="font-size:.72rem;color:var(--muted)">min</span>';
    html+='<button class="bt bt-o" onclick="setSchedule('+z.id+')">Set</button>';
    html+='</div></div>';
  });
  $('pg-irrigation').innerHTML=html;
}

function pad(n){return (n<10?'0':'')+n}

// --- Lighting page ---
function renderLighting(s){
  const lt=s.lighting;
  if(isFocusedInside('pg-lighting'))return;
  if(!lt){$('pg-lighting').innerHTML='<div class="card"><span class="ttl">Lighting module not active</span></div>';return;}
  let html='';
  // Scene buttons
  html+='<div class="card"><span class="ttl">Scenes</span><div class="fr">';
  html+='<button class="bt bt-g" onclick="api(\'lighting/all-on\')">All ON</button>';
  html+='<button class="bt bt-r" onclick="api(\'lighting/all-off\')">All OFF</button>';
  html+='</div></div>';
  // Channels
  const chs=lt.state||[];
  chs.forEach(c=>{
    html+='<div class="card"><div class="hdr"><span class="ttl">Ch '+c.id+' • GPIO'+c.pin+'</span>';
    html+='<span class="st '+ (c.state?'st-run':'st-off') +'">'+ (c.state?'ON':'OFF') +'</span></div>';
    html+='<div style="font-size:.78rem;color:var(--muted);margin-bottom:6px">Schedule: '+(c.scheduleEnabled?c.onTime+' ➔ '+c.offTime:'Disabled')+'</div>';
    html+='<button class="bt '+ (c.state?'bt-r':'bt-g') +'" onclick="api(\'lighting/toggle?id='+c.id+'\')">Toggle</button>';
    html+='</div>';
  });
  $('pg-lighting').innerHTML=html;
}

// --- Logs page ---
function renderLogs(l){
  if(isFocusedInside('pg-logs-scroll'))return;
  if(!l){$('pg-logs').innerHTML='<div class="card"><span class="ttl">Loading logs...</span></div>';return;}
  const events=l.events||[];
  let html='<div class="logs">';
  if(events.length===0)html+='<div class="e" style="color:var(--muted)">No events yet</div>';
  events.forEach(e=>{
    const m=Math.floor(e.ts/60000); const s=Math.floor((e.ts%60000)/1000);
    html+='<div class="e"><span class="tp">'+e.type+':'+e.state+'</span><span class="tm">+'+m+'m'+s+'s</span></div>';
  });
  html+='</div><div id="pg-logs-scroll"></div><div style="font-size:.72rem;color:var(--muted);margin-top:4px">'+l.count+' events in ring buffer</div>';
  $('pg-logs').innerHTML=html;
}

// --- Settings page (static with live inputs) ---
function renderSettings(){
  $('pg-settings').innerHTML=
    '<div class="set"><h3>Wi-Fi Setup</h3><div class="fr">'+
    '<input id="ssid-in" placeholder="SSID" style="flex:1">'+
    '<input id="pass-in" type="password" placeholder="Password" style="flex:1">'+
    '<button class="bt bt-o" onclick="saveWifi()">Save &amp; Restart</button></div></div>'+
    '<div class="set"><h3>RTC Clock</h3><div class="fr">'+
    '<input id="rtc-y" type="number" value="2025" style="width:56px" placeholder="Y">'+
    '<input id="rtc-mo" type="number" value="1" min="1" max="12" style="width:44px" placeholder="M">'+
    '<input id="rtc-d" type="number" value="1" min="1" max="31" style="width:44px" placeholder="D">'+
    '<input id="rtc-h" type="number" value="12" min="0" max="23" style="width:44px" placeholder="H">'+
    '<input id="rtc-mi" type="number" value="0" min="0" max="59" style="width:44px" placeholder="M">'+
    '<button class="bt bt-o" onclick="setClock()">Set</button></div></div>'+
    '<div class="set"><h3>Add Zone</h3><div class="fr">'+
    '<input id="az-id" type="number" value="1" min="1" max="4" style="width:44px" placeholder="ID">'+
    '<input id="az-pin" type="number" value="26" min="0" max="39" style="width:54px" placeholder="GPIO">'+
    '<button class="bt bt-o" onclick="addZone()">Add</button></div></div>'+
    '<div class="set"><h3>Server Sync</h3>'+
    '<div class="fr" style="margin-bottom:6px"><input id="sync-url" placeholder="Server URL" style="flex:1;font-size:.78rem" value="'+getSyncUrl()+'"></div>'+
    '<div class="fr" style="margin-bottom:6px"><input id="sync-key" placeholder="API Key" style="flex:1;font-size:.78rem" value="'+getSyncKey()+'"></div>'+
    '<div class="fr" style="margin-bottom:6px"><input id="sync-devid" placeholder="Device ID" style="flex:1;font-size:.78rem" value="'+getSyncDevId()+'"></div>'+
    '<div style="font-size:.7rem;color:var(--muted);margin-bottom:6px">Status: '+getSyncStatus()+'</div>'+
    '<div class="fr">'+
    '<button class="bt bt-g" onclick="saveSync()">Save &amp; Connect</button>'+
    '<button class="bt bt-o" onclick="api(\'sync/now\')">Test Sync</button></div></div>'+
}

// --- Actions (no page refresh) ---
function setSchedule(id){
  var tEl=document.getElementById('sch-t'+id);
  var dEl=document.getElementById('sch-d'+id);
  var t=tEl.value;
  var d=dEl.value;
  if(!t||!d){alert('Set time and duration first');return;}
  // Blur any active input so focus guard won't block next refresh
  if(document.activeElement)document.activeElement.blur();
  const [h,m]=t.split(':');
  var url='/api/zone/schedule?id='+id+'&hour='+h+'&minute='+m+'&dur='+d;
  fetch(url,{method:'POST'}).then(function(r){return r.json()}).then(function(j){
    if(j.ok){refresh();}
    else {alert('Failed');}
  });
}
function saveWifi(){
  const ss=document.getElementById('ssid-in').value;
  const pw=document.getElementById('pass-in').value;
  if(!ss){alert('Please enter SSID');return;}
  fetch('/api/config/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ss)+'&password='+encodeURIComponent(pw)})
    .then(()=>setTimeout(()=>location.reload(),4000));
}
function setClock(){
  const q=['year','month','day','hour','minute'].map(k=>k+'='+document.getElementById('rtc-'+({'minute':'mi','hour':'h','month':'mo','day':'d','year':'y'})[k]).value).join('&');
  fetch('/api/config/time?'+q,{method:'POST'}).then(refresh);
}
function addZone(){
  const id=document.getElementById('az-id').value;
  const pin=document.getElementById('az-pin').value;
  api('zone/add?id='+id+'&pin='+pin);

// --- Sync helpers ---
function getSyncUrl(){ return st&&st.sync?st.sync.url||'':'' }
function getSyncKey(){ return st&&st.sync?st.sync.apiKey||'':'' }
function getSyncDevId(){ return st&&st.sync?(st.sync.deviceId||st.sync.device_id||''):'' }
function getSyncStatus(){ 
  var sy=st&&st.sync?st.sync:null;
  if(!sy||!sy.enabled)return 'Not configured';
  return (sy.online?'Connected':'Offline')+' | Pending: '+(sy.pending||0);
}
function saveSync(){
  var u=document.getElementById('sync-url').value.trim();
  var k=document.getElementById('sync-key').value.trim();
  var d=document.getElementById('sync-devid').value.trim();
  var body='url='+encodeURIComponent(u)+'&key='+encodeURIComponent(k)+'&devid='+encodeURIComponent(d);
  fetch('/api/config/sync',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(r){return r.json()}).then(function(j){
      if(j.ok){refresh();}
    });
  if(document.activeElement)document.activeElement.blur();
}

// --- Tab switching ---
document.getElementById('tabs').addEventListener('click',e=>{
  const tb=e.target.closest('.tab');
  if(!tb)return;
  page=tb.dataset.pg;
  document.querySelectorAll('.tab').forEach(t=>t.classList.toggle('on',t===tb));
  document.querySelectorAll('[id^="pg-"]').forEach(p=>p.classList.toggle('hidden',p.id!=='pg-'+page));
  if(page==='settings')renderSettings();
});

// --- Init ---
renderSettings();
refresh();
setInterval(refresh,4000);
setInterval(function(){
  if(page==='logs')fetch('/api/logs').then(r=>r.json()).then(l=>renderLogs(l)).catch(()=>{});
},5000);
</script>
</body></html>)HTML";
}

// =========== Route registration ===========

void WebApp::begin() {
  server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleDashboard(r); });
  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });
  server.on("/api/logs", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLogs(r); });
  server.on("/health", HTTP_GET, [this](AsyncWebServerRequest* r) { handleHealth(r); });
  server.on("/api/config/time", HTTP_POST, [this](AsyncWebServerRequest* r) { handleConfigTime(r); });
  server.on("/api/config/wifi", HTTP_POST, [this](AsyncWebServerRequest* r) { handleConfigWifi(r); });
  server.on("/api/config/sync", HTTP_POST, [this](AsyncWebServerRequest* r) { handleConfigSync(r); });

#ifdef MODULE_IRRIGATION
  auto zoneon = [this](AsyncWebServerRequest* r) { handleZoneOn(r); };
  auto zoneoff = [this](AsyncWebServerRequest* r) { handleZoneOff(r); };
  auto zoneadd = [this](AsyncWebServerRequest* r) { handleZoneAdd(r); };
  auto zonesch = [this](AsyncWebServerRequest* r) { handleZoneSchedule(r); };
  auto pumpon = [this](AsyncWebServerRequest* r) { handlePumpOn(r); };
  auto pumpoff = [this](AsyncWebServerRequest* r) { handlePumpOff(r); };
  auto alloff = [this](AsyncWebServerRequest* r) { handleAllOff(r); };

  server.on("/api/zone/on", HTTP_POST, zoneon);
  server.on("/api/zone/off", HTTP_POST, zoneoff);
  server.on("/api/zone/add", HTTP_POST, zoneadd);
  server.on("/api/zone/schedule", HTTP_POST, zonesch);
  server.on("/api/pump/on", HTTP_POST, pumpon);
  server.on("/api/pump/off", HTTP_POST, pumpoff);
  server.on("/api/e", HTTP_POST, alloff);
#endif

#ifdef MODULE_LIGHTING
  server.on("/api/lighting/toggle", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightToggle(r); });
  server.on("/api/lighting/all-on", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightAllOn(r); });
  server.on("/api/lighting/all-off", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightAllOff(r); });
#endif

  // Sync endpoint
  server.on("/api/sync/now", HTTP_POST, [this](AsyncWebServerRequest* r) {
    syncTriggerNow();
    sendJson(r, "{\"ok\":true}");
  });

  server.begin();
}

void WebApp::update() {}
