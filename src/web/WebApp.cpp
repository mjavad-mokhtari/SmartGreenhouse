#include "web/WebApp.h"

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
  AsyncWebServerResponse* response = request->beginResponse(code, "application/json", body);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

void WebApp::sendJsonObj(AsyncWebServerRequest* request, JsonDocument& doc, int code) {
  String body;
  serializeJson(doc, body);
  sendJson(request, body, code);
}

String WebApp::buildStatusJson() {
  DynamicJsonDocument doc(1024);
  DateTime now = rtc.now();
  doc["time"] = String(now.year()) + "-" +
    (now.month() < 10 ? "0" : "") + String(now.month()) + "-" +
    (now.day() < 10 ? "0" : "") + String(now.day()) + " " +
    (now.hour() < 10 ? "0" : "") + String(now.hour()) + ":" +
    (now.minute() < 10 ? "0" : "") + String(now.minute()) + ":" +
    (now.second() < 10 ? "0" : "") + String(now.second());
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
  if (!wifi.connected())
    h["wifiDisconnected"] = true;
  else
    h["wifiDisconnected"] = false;
  h["rtcValid"] = (now.year() >= 2020);

#ifdef MODULE_IRRIGATION
  irrigation.appendStatus(doc.as<JsonObject>());
#endif

#ifdef MODULE_LIGHTING
  if (!doc.containsKey("lighting"))
    lighting.appendStatus(doc.as<JsonObject>());
#endif

  String out;
  serializeJson(doc, out);
  return out;
}

void WebApp::handleStatus(AsyncWebServerRequest* request) {
  sendJson(request, buildStatusJson());
}

void WebApp::handleHealth(AsyncWebServerRequest* request) {
  request->send(200, "application/json", "{\"status\":\"ok\"}");
}

void WebApp::handleDashboard(AsyncWebServerRequest* request) {
  request->send(200, "text/html", buildFullDashboard());
}

#ifdef MODULE_IRRIGATION
void WebApp::handleZoneOn(AsyncWebServerRequest* request) {
  String idStr = request->arg("id");
  if (idStr.length() == 0 && request->hasParam("id", true))
    idStr = request->getParam("id", true)->value();
  String durStr = request->arg("dur");
  if (durStr.length() == 0 && request->hasParam("dur", true))
    durStr = request->getParam("dur", true)->value();

  uint8_t id = idStr.toInt();
  uint16_t dur = durStr.length() > 0 ? durStr.toInt() : 15;
  if (dur == 0) dur = 15;

  int idx = irrigation.findZone(id);
  if (idx >= 0 && irrigation.start(idx, dur, "HTTP manual start"))
    sendJson(request, "{\"result\":\"started\"}");
  else
    sendJson(request, "{\"result\":\"error\",\"msg\":\"zone not found\"}", 400);
}

void WebApp::handleZoneOff(AsyncWebServerRequest* request) {
  String idStr = request->arg("id");
  if (idStr.length() == 0 && request->hasParam("id", true))
    idStr = request->getParam("id", true)->value();
  uint8_t id = idStr.toInt();
  int idx = irrigation.findZone(id);
  if (idx >= 0 && irrigation.stop(idx, "HTTP manual stop"))
    sendJson(request, "{\"result\":\"stopped\"}");
  else
    sendJson(request, "{\"result\":\"error\",\"msg\":\"zone not found\"}", 400);
}

void WebApp::handleZoneAdd(AsyncWebServerRequest* request) {
  String idStr = request->arg("id");
  String pinStr = request->arg("pin");
  if (idStr.length() == 0 && request->hasParam("id", true))
    idStr = request->getParam("id", true)->value();
  if (pinStr.length() == 0 && request->hasParam("pin", true))
    pinStr = request->getParam("pin", true)->value();

  uint8_t id = idStr.toInt();
  uint8_t pin = pinStr.toInt();

  if (irrigation.addZone(id, pin))
    sendJson(request, "{\"result\":\"added\"}");
  else
    sendJson(request, "{\"result\":\"error\",\"msg\":\"invalid id/pin or conflict\"}", 400);
}

void WebApp::handleZoneSchedule(AsyncWebServerRequest* request) {
  String idStr = request->arg("id");
  String hourStr = request->arg("hour");
  String minStr = request->arg("minute");
  if (minStr.length() == 0) minStr = request->arg("min");
  String durStr = request->arg("dur");
  if (durStr.length() == 0) durStr = request->arg("duration");

  if (idStr.length() == 0 && request->hasParam("id", true))
    idStr = request->getParam("id", true)->value();
  if (hourStr.length() == 0 && request->hasParam("hour", true))
    hourStr = request->getParam("hour", true)->value();
  if (minStr.length() == 0 && request->hasParam("minute", true))
    minStr = request->getParam("minute", true)->value();
  if (durStr.length() == 0 && request->hasParam("dur", true))
    durStr = request->getParam("dur", true)->value();

  uint8_t id = idStr.toInt();
  uint8_t hour = hourStr.toInt();
  uint8_t minute = minStr.toInt();
  uint16_t dur = durStr.toInt();

  int idx = irrigation.findZone(id);
  if (idx >= 0 && irrigation.setSchedule(idx, hour, minute, dur))
    sendJson(request, "{\"result\":\"scheduled\"}");
  else
    sendJson(request, "{\"result\":\"error\",\"msg\":\"invalid params\"}", 400);
}

void WebApp::handlePumpOn(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  irrigation.setPump(true, "HTTP manual start");
  sendJson(request, "{\"pump\":\"on\"}");
}

void WebApp::handlePumpOff(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  for (uint8_t i = 0; i < irrigation.zoneCount(); i++)
    irrigation.stop(i, "Pump stop");
  irrigation.setPump(false, "HTTP manual stop");
  sendJson(request, "{\"pump\":\"off\"}");
}

void WebApp::handleAllOff(AsyncWebServerRequest* request) {
  irrigation.clearPumpOverride();
  for (uint8_t i = 0; i < irrigation.zoneCount(); i++)
    irrigation.stop(i, "Emergency stop");
  irrigation.setPump(false, "Emergency stop");
  sendJson(request, "{\"result\":\"allOff\"}");
}
#endif

#ifdef MODULE_LIGHTING
void WebApp::handleLightToggle(AsyncWebServerRequest* request) {
  String idStr = request->arg("id");
  if (idStr.length() == 0 && request->hasParam("id", true))
    idStr = request->getParam("id", true)->value();
  uint8_t id = idStr.toInt();
  if (lighting.toggleChannel(id))
    sendJson(request, "{\"result\":\"ok\"}");
  else
    sendJson(request, "{\"result\":\"error\"}", 400);
}

void WebApp::handleLightAllOn(AsyncWebServerRequest* request) {
  lighting.allOn();
  sendJson(request, "{\"result\":\"allOn\"}");
}

void WebApp::handleLightAllOff(AsyncWebServerRequest* request) {
  lighting.allOff();
  sendJson(request, "{\"result\":\"allOff\"}");
}
#endif

void WebApp::handleConfigTime(AsyncWebServerRequest* request) {
  String y = request->arg("year");
  String mo = request->arg("month");
  String d = request->arg("day");
  String h = request->arg("hour");
  String mi = request->arg("minute");
  String s = request->arg("second");

  if (y.length() > 0 && mo.length() > 0 && d.length() > 0) {
    rtc.set(DateTime(y.toInt(), mo.toInt(), d.toInt(),
      h.length() > 0 ? h.toInt() : 0,
      mi.length() > 0 ? mi.toInt() : 0,
      s.length() > 0 ? s.toInt() : 0));
    sendJson(request, "{\"result\":\"ok\"}");
  } else {
    sendJson(request, "{\"result\":\"error\"}", 400);
  }
}

void WebApp::handleConfigWifi(AsyncWebServerRequest* request) {
  String ssid = request->arg("ssid");
  String pass = request->arg("password");
  if (ssid.length() == 0 && request->hasParam("ssid", true))
    ssid = request->getParam("ssid", true)->value();
  if (pass.length() == 0 && request->hasParam("password", true))
    pass = request->getParam("password", true)->value();

  if (ssid.length() > 0) {
    wifi.save(ssid, pass);
    sendJson(request, "{\"result\":\"saved\",\"restarting\":true}");
    delay(1500);
    ESP.restart();
  } else {
    sendJson(request, "{\"result\":\"error\",\"msg\":\"missing ssid\"}", 400);
  }
}

String WebApp::buildFullDashboard() {
  return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SGH – Smart Greenhouse</title>
  <style>
    *,*::before,*::after{box-sizing:border-box}
    body{margin:0;background:#1a1d1f;color:#e4e6e1;font:15px/1.5 system-ui,sans-serif}
    main{max-width:960px;margin:auto;padding:24px 16px}
    header{display:flex;justify-content:space-between;align-items:end;border-bottom:2px solid #3a4d3e;padding-bottom:16px;margin-bottom:20px}
    h1{font:clamp(1.8rem,5vw,3rem) Georgia,serif;margin:0;color:#7cb342}
    #clock{font-size:.9rem;color:#9ba89e}
    .nav{display:flex;gap:6px;margin:0 0 22px;flex-wrap:wrap}
    .nav button{border:1px solid #3a4d3e;background:#26292b;color:#c8d6c0;padding:8px 18px;border-radius:6px;cursor:pointer;font-size:.88rem}
    .nav button:hover,.nav button.active{background:#2e4a2e;border-color:#7cb342;color:#fff}
    .panel{display:none;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px;margin-bottom:28px}
    .panel.active{display:grid}
    .card{background:#222527;border:1px solid #333a35;padding:18px;border-radius:10px}
    .card h2{margin:0 0 4px;font-size:.85rem;color:#889c88;text-transform:uppercase;letter-spacing:.5px}
    .card .value{font-size:2rem;font-weight:600;color:#7cb342}
    .card .sub{font-size:.82rem;color:#889c88;margin-top:2px}
    .grid-2{display:grid;grid-template-columns:1fr 1fr;gap:12px}
    .btn{padding:8px 16px;border-radius:6px;border:none;cursor:pointer;font-size:.9rem;font-weight:500}
    .btn-on{background:#2e5a3b;color:#fff}
    .btn-off{background:#5a3b2e;color:#fff}
    .btn-danger{background:#8b2e2e;color:#fff}
    .btn-save{background:#2e4a6e;color:#fff}
    .btn:hover{opacity:.85}
    .row{display:flex;align-items:center;gap:8px;margin:10px 0;flex-wrap:wrap}
    label{font-size:.85rem;color:#9ba89e}
    input,select{background:#1a1d1f;border:1px solid #3a4d3e;color:#e4e6e1;padding:6px 10px;border-radius:5px;font-size:.9rem}
    input[type=number]{width:70px}
    .badge{padding:2px 8px;border-radius:12px;font-size:.75rem;font-weight:600}
    .badge-ok{background:#2e5a3b;color:#90ee90}
    .badge-warn{background:#5a4e2e;color:#ffed90}
    .badge-err{background:#5a2e2e;color:#f59090}
    .section{margin-top:28px;border-top:1px solid #333a35;padding-top:18px}
    .section h2{color:#7cb342;font-size:1.1rem;margin-bottom:12px}
  </style>
</head>
<body>
<main>
  <header>
    <h1>SGH</h1>
    <span id="clock">Connecting...</span>
  </header>

  <nav class="nav">
    <button class="active" data-panel="irrigation">Irrigation</button>
    <button data-panel="lighting">Lighting</button>
    <button data-panel="health">System Health</button>
    <button data-panel="settings">Settings</button>
  </nav>

  <!-- Irrigation Panel -->
  <section class="panel active" id="panel-irrigation"></section>

  <!-- Lighting Panel -->
  <section class="panel" id="panel-lighting"></section>

  <!-- Health Panel -->
  <section class="panel" id="panel-health">
    <div class="card"><h2>Wi-Fi</h2><span id="health-wifi" class="badge">-</span><div class="sub" id="health-ip"></div></div>
    <div class="card"><h2>RTC</h2><span id="health-rtc" class="badge">-</span><div class="sub" id="health-time"></div></div>
    <div class="card"><h2>Free Heap</h2><div class="value" style="font-size:1.4rem" id="health-heap">-</div></div>
    <div class="card"><h2>Uptime</h2><div class="value" style="font-size:1.4rem" id="health-uptime">-</div></div>
  </section>

  <!-- Settings Panel -->
  <section class="panel" id="panel-settings">
    <div class="card" style="grid-column:1/-1">
      <h2>Wi-Fi Setup</h2>
      <div class="row"><label>SSID:</label><input id="wifi-ssid" placeholder="network name"><label style="margin-left:12px">Password:</label><input id="wifi-pass" type="password" placeholder="password"></div>
      <div class="row"><button class="btn btn-save" onclick="saveWifi()">Save & Restart</button></div>
    </div>
    <div class="card" style="grid-column:1/-1">
      <h2>RTC Clock</h2>
      <div class="row">
        <label>Year</label><input id="rtc-y" type="number" value="2025">&nbsp;
        <label>Month</label><input id="rtc-mo" type="number" min="1" max="12" value="1">&nbsp;
        <label>Day</label><input id="rtc-d" type="number" min="1" max="31" value="1">&nbsp;
        <label>Hour</label><input id="rtc-h" type="number" min="0" max="23" value="12">&nbsp;
        <label>Min</label><input id="rtc-mi" type="number" min="0" max="59" value="0">
      </div>
      <div class="row"><button class="btn btn-save" onclick="setClock()">Set Clock</button></div>
    </div>
    <div class="card" style="grid-column:1/-1">
      <h2>Add Zone</h2>
      <div class="row">
        <label>Zone ID</label><input id="add-zone-id" type="number" min="1" max="4" value="1">&nbsp;
        <label>GPIO Pin</label><input id="add-zone-pin" type="number" min="0" max="39" value="26">
      </div>
      <div class="row"><button class="btn btn-save" onclick="addZone()">Add Zone</button></div>
    </div>
  </section>

  <!-- Emergency Stop Button (always visible) -->
  <div style="text-align:center;margin:28px 0">
    <button class="btn btn-danger" style="font-size:1.2rem;padding:14px 40px" onclick="emergencyStop()">EMERGENCY STOP</button>
  </div>
</main>

<script>
const navButtons = document.querySelectorAll('.nav button');
navButtons.forEach(b => {
  b.addEventListener('click', () => {
    navButtons.forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
    document.getElementById('panel-' + b.dataset.panel).classList.add('active');
  });
});

let lastStatus = null;

async function refresh() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    lastStatus = s;
    clock.textContent = s.time || 'No RTC';

    // Health
    document.getElementById('health-wifi').textContent = (s.wifi && s.wifi.connected) ? 'CONNECTED' : 'DISCONNECTED';
    document.getElementById('health-wifi').className = 'badge ' + ((s.wifi && s.wifi.connected) ? 'badge-ok' : 'badge-err');
    document.getElementById('health-ip').textContent = (s.wifi && s.wifi.ip) || '-';
    document.getElementById('health-rtc').textContent = (s.rtc && s.rtc.valid) ? 'OK' : 'INVALID';
    document.getElementById('health-rtc').className = 'badge ' + ((s.rtc && s.rtc.valid) ? 'badge-ok' : 'badge-err');
    document.getElementById('health-time').textContent = s.time || '-';
    document.getElementById('health-heap').textContent = s.freeHeap ? (s.freeHeap / 1024).toFixed(1) + ' KB' : '-';
    document.getElementById('health-uptime').textContent = s.uptime ? Math.floor(s.uptime / 1000) + 's' : '-';

    // Irrigation
    renderIrrigation(s);
    // Lighting
    renderLighting(s);
  } catch(e) { /* retry next interval */ }
}

function renderIrrigation(s) {
  const irr = s.irrigation;
  if (!irr) { document.getElementById('panel-irrigation').innerHTML = '<div class="card"><p>Irrigation module not active</p></div>'; return; }
  let html = '';

  // Pump card
  html += '<div class="card"><h2>Pump GPI' + irr.pumpGpio + '</h2>';
  html += '<div class="value">' + (irr.pumpOn ? 'ON' : 'OFF') + '</div>';
  html += '<div class="sub">Manual override: ' + (irr.pumpManualOverride ? 'Yes' : 'No') + '</div>';
  html += '<div class="row" style="margin-top:10px">';
  html += '<button class="btn btn-on" onclick="pumpOn()">Pump ON</button>';
  html += '<button class="btn btn-off" onclick="pumpOff()">Pump OFF</button>';
  html += '</div></div>';

  // Soil sensor card
  html += '<div class="card"><h2>Soil Sensor</h2>';
  html += '<div class="value">' + irr.soilPercent + '%</div>';
  html += '<div class="sub">Raw: ' + irr.soilRaw + '</div></div>';

  // Zone cards
  const zones = irr.zones || [];
  for (const z of zones) {
    html += '<div class="card"><h2>Zone ' + z.id + ' – GPI' + z.pin + '</h2>';
    html += '<div class="value" style="color:' + (z.running ? '#f59090' : '#7cb342') + '">' + (z.running ? 'RUNNING' : 'IDLE') + '</div>';
    html += '<div class="sub">Enabled: ' + (z.enabled ? 'Yes' : 'No') + ' | Next: ' + (z.nextRun || 'None') + '</div>';
    if (z.running) html += '<div class="sub">Remaining: ' + z.remainingSec + 's</div>';
    html += '<div class="row" style="margin-top:10px">';
    html += '<button class="btn btn-on" onclick="zoneOn(' + z.id + ')">ON</button>';
    html += '<button class="btn btn-off" onclick="zoneOff(' + z.id + ')">OFF</button>';
    html += '</div>';
    // Schedule sub-form
    html += '<div class="row" style="margin-top:6px">';
    html += '<input id="sch-h' + z.id + '" type="number" min="0" max="23" value="' + (z.hour||8) + '" style="width:50px" placeholder="H">&nbsp;:&nbsp;';
    html += '<input id="sch-m' + z.id + '" type="number" min="0" max="59" value="' + (z.minute||0) + '" style="width:50px" placeholder="M">&nbsp;';
    html += '<input id="sch-d' + z.id + '" type="number" min="1" max="120" value="' + (z.duration||15) + '" style="width:55px" placeholder="min">&nbsp;';
    html += '<button class="btn btn-save" onclick="setSchedule(' + z.id + ')">Set</button>';
    html += '</div></div>';
  }

  document.getElementById('panel-irrigation').innerHTML = html;
}

function renderLighting(s) {
  const l = s.lighting;
  if (!l) { document.getElementById('panel-lighting').innerHTML = '<div class="card"><p>Lighting module not active</p></div>'; return; }
  let html = '';
  const channels = l.state || [];
  for (const c of channels) {
    html += '<div class="card"><h2>Channel ' + c.id + ' – GPI' + c.pin + '</h2>';
    html += '<div class="value" style="color:' + (c.state ? '#f59090' : '#7cb342') + '">' + (c.state ? 'ON' : 'OFF') + '</div>';
    html += '<div class="sub">Schedule: ' + (c.scheduleEnabled ? c.onTime + ' → ' + c.offTime : 'Disabled') + '</div>';
    html += '<div class="row" style="margin-top:10px">';
    html += '<button class="btn btn-on" onclick="lightToggle(' + c.id + ')">Toggle</button>';
    html += '</div></div>';
  }
  // Scene buttons
  html += '<div class="card" style="grid-column:1/-1"><h2>Scenes</h2>';
  html += '<div class="row">';
  html += '<button class="btn btn-on" onclick="lightAllOn()">All Lights ON</button>';
  html += '<button class="btn btn-off" onclick="lightAllOff()">All Lights OFF</button>';
  html += '</div></div>';

  document.getElementById('panel-lighting').innerHTML = html;
}

// API calls
async function api(path) { try { await fetch(path); refresh(); } catch(e) {} }
function zoneOn(id) { api('/api/zone/on?id=' + id + '&dur=15'); }
function zoneOff(id) { api('/api/zone/off?id=' + id); }
function pumpOn() { api('/api/pump/on'); }
function pumpOff() { api('/api/pump/off'); }
function emergencyStop() { api('/api/all-off'); }
function lightToggle(id) { api('/lighting/toggle?id=' + id); }
function lightAllOn() { api('/lighting/all-on'); }
function lightAllOff() { api('/lighting/all-off'); }
function addZone() {
  const id = document.getElementById('add-zone-id').value;
  const pin = document.getElementById('add-zone-pin').value;
  api('/api/zone/add?id=' + id + '&pin=' + pin);
}
function setSchedule(id) {
  const h = document.getElementById('sch-h' + id).value;
  const m = document.getElementById('sch-m' + id).value;
  const d = document.getElementById('sch-d' + id).value;
  api('/api/zone/schedule?id=' + id + '&hour=' + h + '&minute=' + m + '&dur=' + d);
}
function saveWifi() {
  const ssid = document.getElementById('wifi-ssid').value;
  const pass = document.getElementById('wifi-pass').value;
  fetch('/api/config/wifi', {method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass)})
    .then(() => setTimeout(() => location.reload(), 4000));
}
function setClock() {
  const y = document.getElementById('rtc-y').value, mo = document.getElementById('rtc-mo').value, d = document.getElementById('rtc-d').value;
  const h = document.getElementById('rtc-h').value, mi = document.getElementById('rtc-mi').value;
  api('/api/config/time?year='+y+'&month='+mo+'&day='+d+'&hour='+h+'&minute='+mi);
}

refresh();
setInterval(refresh, 3000);
</script>
</body></html>)HTML";
}

void WebApp::begin() {
  server.on("/", HTTP_GET, [this](AsyncWebServerRequest* r) { handleDashboard(r); });
  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* r) { handleStatus(r); });
  server.on("/health", HTTP_GET, [this](AsyncWebServerRequest* r) { handleHealth(r); });
  server.on("/api/config/time", HTTP_POST, [this](AsyncWebServerRequest* r) { handleConfigTime(r); });
  server.on("/api/config/wifi", HTTP_POST, [this](AsyncWebServerRequest* r) { handleConfigWifi(r); });

#ifdef MODULE_IRRIGATION
  server.on("/api/zone/on", HTTP_POST, [this](AsyncWebServerRequest* r) { handleZoneOn(r); });
  server.on("/api/zone/off", HTTP_POST, [this](AsyncWebServerRequest* r) { handleZoneOff(r); });
  server.on("/api/zone/add", HTTP_POST, [this](AsyncWebServerRequest* r) { handleZoneAdd(r); });
  server.on("/api/zone/schedule", HTTP_POST, [this](AsyncWebServerRequest* r) { handleZoneSchedule(r); });
  server.on("/api/zone/schedule", HTTP_GET, [this](AsyncWebServerRequest* r) { handleZoneSchedule(r); });
  server.on("/api/pump/on", HTTP_POST, [this](AsyncWebServerRequest* r) { handlePumpOn(r); });
  server.on("/api/pump/off", HTTP_POST, [this](AsyncWebServerRequest* r) { handlePumpOff(r); });
  server.on("/api/all-off", HTTP_POST, [this](AsyncWebServerRequest* r) { handleAllOff(r); });
  // Also support GET for convenience (from dashboard links)
  server.on("/api/zone/on", HTTP_GET, [this](AsyncWebServerRequest* r) { handleZoneOn(r); });
  server.on("/api/zone/off", HTTP_GET, [this](AsyncWebServerRequest* r) { handleZoneOff(r); });
  server.on("/api/pump/on", HTTP_GET, [this](AsyncWebServerRequest* r) { handlePumpOn(r); });
  server.on("/api/pump/off", HTTP_GET, [this](AsyncWebServerRequest* r) { handlePumpOff(r); });
  server.on("/api/all-off", HTTP_GET, [this](AsyncWebServerRequest* r) { handleAllOff(r); });
#endif

#ifdef MODULE_LIGHTING
  server.on("/lighting/toggle", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLightToggle(r); });
  server.on("/lighting/all-on", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLightAllOn(r); });
  server.on("/lighting/all-off", HTTP_GET, [this](AsyncWebServerRequest* r) { handleLightAllOff(r); });
  server.on("/api/lighting/toggle", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightToggle(r); });
  server.on("/api/lighting/all-on", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightAllOn(r); });
  server.on("/api/lighting/all-off", HTTP_POST, [this](AsyncWebServerRequest* r) { handleLightAllOff(r); });
#endif

  server.begin();
}

void WebApp::update() {
  // AsyncWebServer handles requests in the background
}
