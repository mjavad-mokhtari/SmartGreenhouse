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

void WebApp::begin() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/html", dashboard()); });
  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) { sendStatus(request); });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request) { request->send(200, "text/plain", "OK"); });
  server.on("/api/config/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (!request->hasParam("ssid", true)) { request->send(400, "application/json", "{\"ok\":false}"); return; }
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : String();
    wifi.save(request->getParam("ssid", true)->value(), password);
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.begin();
}

void WebApp::sendStatus(AsyncWebServerRequest* request) {
  DynamicJsonDocument document(768);
  document["time"] = rtc.now().timestamp(DateTime::TIMESTAMP_FULL);
  document["wifi"]["connected"] = wifi.connected();
  document["wifi"]["network"] = wifi.network();
  document["wifi"]["ip"] = wifi.ip();
#ifdef MODULE_IRRIGATION
  irrigation.appendStatus(document.as<JsonObject>());
#endif
#ifdef MODULE_LIGHTING
  lighting.appendStatus(document.as<JsonObject>());
#endif
  String body;
  serializeJson(document, body);
  request->send(200, "application/json", body);
}

const char* WebApp::dashboard() {
  return R"HTML(<!doctype html><html lang="en"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SGH</title><style>body{margin:0;background:#f2f0e9;color:#20251f;font:16px ui-sans-serif,system-ui,sans-serif}main{max-width:900px;margin:auto;padding:28px}header{display:flex;justify-content:space-between;align-items:end;border-bottom:2px solid #20251f;padding-bottom:18px}h1{font:clamp(2rem,6vw,4rem) Georgia,serif;margin:0}.tabs{display:flex;gap:8px;margin:24px 0;flex-wrap:wrap}button{border:1px solid #20251f;background:#f8f7f2;padding:10px 16px;border-radius:4px;cursor:pointer}button.active{background:#d6e85e}.panel{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px}.card{background:#fff;border:1px solid #d9d6cb;padding:18px;border-radius:6px}.value{font-size:2rem;color:#466b56}small{color:#6c7169}</style><main><header><h1>SGH</h1><small id="clock">Connecting...</small></header><nav class="tabs"><button class="active">Irrigation</button><button>Lighting</button><button>System health</button><button>Settings</button></nav><section class="panel"><article class="card"><small>Local controller</small><div class="value" id="network">...</div><p>All services run locally.</p></article><article class="card"><small>Module status</small><div class="value" id="modules">...</div><p>No cloud connection required.</p></article></section></main><script>async function refresh(){const s=await fetch('/api/status').then(r=>r.json());clock.textContent=s.time;network.textContent=s.wifi.network+' / '+s.wifi.ip;modules.textContent=Object.keys(s).filter(k=>k==='irrigation'||k==='lighting').join(' + ')||'core only'}refresh();setInterval(refresh,3000)</script></html>)HTML";
}