#include <Arduino.h>
#include <esp_system.h>
#include <ArduinoOTA.h>
#include "core/Services.h"
#ifdef MODULE_IRRIGATION
#include "modules/irrigation/IrrigationService.h"
#endif
#ifdef MODULE_LIGHTING
#include "modules/lighting/LightingService.h"
#endif
#include "web/WebApp.h"
#include "sync.h"

// --- Core services ---
RtcManager rtc;
StatusLed led(2);
PreferencesStore sysStore("system");
PreferencesStore irrStore("irrigation");
WifiManager wifi(irrStore);

#ifdef MODULE_IRRIGATION
IrrigationService irrigation(rtc, led, irrStore);
#endif

#ifdef MODULE_LIGHTING
LightingService lighting(rtc, led);
#endif

WebApp webApp(rtc, wifi
#ifdef MODULE_IRRIGATION
  , irrigation
#endif
#ifdef MODULE_LIGHTING
  , lighting
#endif
);

void setup() {
  Serial.begin(115200);

  led.begin();
  rtc.begin();

#ifdef MODULE_IRRIGATION
  irrigation.begin();
#endif

#ifdef MODULE_LIGHTING
  lighting.begin();
#endif

  wifi.begin();

  // --- OTA Setup ---
  ArduinoOTA.setHostname("smartgreenhome");
  ArduinoOTA.setPassword("greenhouse_ota");
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Start updating firmware...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Update complete. Rebooting...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Ready. Password: greenhouse_ota");

  // Load sync config
  syncLoadConfig();
  if (wifi.connected() && !wifi.isAccessPoint()) {
    WiFi.setAutoReconnect(true);
  }

  webApp.begin();

  Serial.println("=== Smart Greenhouse Controller ===");
  Serial.printf("IP: %s\n", wifi.ip().c_str());
  Serial.printf("Network: %s\n", wifi.network().c_str());
  Serial.println("Ready. Type HELP for CLI commands.");
}

// --- CLI ---
void handleCLI() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  String u = cmd;
  u.toUpperCase();

  if (u == "HELP") {
    Serial.println("ADD <id> <gpio> | SET <id> <HH:MM> <min> | ENABLE <id> | DISABLE <id>");
    Serial.println("ON <id> <min> | OFF <id> | PUMP ON/OFF/STATUS | RELAY TEST | STATUS | SYNC");
    return;
  }

  if (u == "STATUS") {
    Serial.printf("Time: %04d-%02d-%02d %02d:%02d:%02d\n",
      rtc.now().year(), rtc.now().month(), rtc.now().day(),
      rtc.now().hour(), rtc.now().minute(), rtc.now().second());
#ifdef MODULE_IRRIGATION
    Serial.printf("Zones: %u | Pump: %s | Pending events: %u\n",
      irrigation.zoneCount(), irrigation.pumpIsOn() ? "ON" : "OFF", pendingCount());
#endif
    return;
  }

#ifdef MODULE_IRRIGATION
  if (u == "RELAY TEST") {
    Serial.println("Relay Test: GPIO25 (pump) ON 3s");
    irrigation.setPump(true, "relay test");
    delay(3000);
    irrigation.setPump(false, "relay test done");
    Serial.println("Relay Test: GPIO26 (zone 1) ON 3s");
    // Find zone 1 and start briefly
    int zi = irrigation.findZone(1);
    if (zi >= 0) { irrigation.start(zi, 1, "relay test"); delay(3000); irrigation.stop(zi, "relay test done"); }
    Serial.println("Relay Test: complete");
    return;
  }

  if (u == "PUMP ON") {
    irrigation.clearPumpOverride();
    irrigation.setPump(true, "CLI manual start");
    return;
  }
  if (u == "PUMP OFF") {
    irrigation.clearPumpOverride();
    for (uint8_t i = 0; i < irrigation.zoneCount(); i++)
      irrigation.stop(i, "Pump stop");
    irrigation.setPump(false, "CLI manual stop");
    return;
  }
  if (u == "PUMP STATUS") {
    Serial.printf("Pump: %s | GPIO %d\n",
      irrigation.pumpIsOn() ? "ON" : "OFF",
      IrrigationService::RELAY_PINS[IrrigationService::PUMP_RELAY_CHANNEL]);
    return;
  }

  int a, b;
  char tm[6];
  if (sscanf(u.c_str(), "ADD %d %d", &a, &b) == 2) {
    if (irrigation.addZone(a, (uint8_t)b))
      Serial.printf("Zone %d added on GPIO %d\n", a, b);
    else
      Serial.printf("ERROR: ADD rejected (duplicate/full/reserved GPIO %d)\n", b);
    return;
  }
  if (sscanf(u.c_str(), "ON %d %d", &a, &b) == 2) {
    int zi = irrigation.findZone(a);
    if (irrigation.start(zi, b, "MANUAL"))
      Serial.printf("Zone %d ON for %d min\n", a, b);
    else
      Serial.println("ERROR: Zone not found or invalid duration");
    return;
  }
  if (sscanf(u.c_str(), "OFF %d", &a) == 1) {
    int zi = irrigation.findZone(a);
    if (irrigation.stop(zi, "Manual stop"))
      Serial.printf("Zone %d OFF\n", a);
    else
      Serial.println("ERROR: Zone not found");
    return;
  }
  if (sscanf(u.c_str(), "ENABLE %d", &a) == 1) {
    irrigation.enableZone(irrigation.findZone(a));
    return;
  }
  if (sscanf(u.c_str(), "DISABLE %d", &a) == 1) {
    irrigation.disableZone(irrigation.findZone(a));
    return;
  }
  if (sscanf(u.c_str(), "SET %d %5s %d", &a, tm, &b) == 3) {
    int hh, mm;
    int zi = irrigation.findZone(a);
    if (zi >= 0 && sscanf(tm, "%d:%d", &hh, &mm) == 2) {
      if (irrigation.setSchedule(zi, hh, mm, b))
        Serial.printf("Schedule set: Zone %d at %02d:%02d for %d min\n", a, hh, mm, b);
    }
    return;
  }
#endif

#ifdef MODULE_LIGHTING
  if (sscanf(u.c_str(), "LIGHT %d %d", &a, &b) == 2) {
    lighting.setChannel(a, b != 0);
    Serial.printf("Lighting channel %d: %s\n", a, b ? "ON" : "OFF");
    return;
  }
  if (u == "LIGHT ALL ON") { lighting.allOn(); return; }
  if (u == "LIGHT ALL OFF") { lighting.allOff(); return; }
#endif

  if (u == "SYNC") {
    syncTriggerNow();
    Serial.println("Sync triggered.");
    return;
  }
}

String buildServerStatusJson() {
  DynamicJsonDocument doc(2048);
  JsonObject root = doc.to<JsonObject>();
  DateTime now = rtc.now();
  char timeBuf[20];
  snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
    now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  root["time"] = timeBuf;
  root["uptime"] = millis();
  root["freeHeap"] = ESP.getFreeHeap();

  JsonObject w = root.createNestedObject("wifi");
  w["connected"] = wifi.connected();
  w["ip"] = wifi.ip();
  w["network"] = wifi.network();
  w["apMode"] = wifi.isAccessPoint();

  JsonObject r = root.createNestedObject("rtc");
  r["available"] = rtc.isAvailable();
  r["valid"] = (now.year() >= 2020);
  r["year"] = now.year();
  r["month"] = now.month();
  r["day"] = now.day();
  r["hour"] = now.hour();
  r["minute"] = now.minute();
  r["second"] = now.second();

  JsonObject h = root.createNestedObject("health");
  h["wifiDisconnected"] = !wifi.connected();
  h["rtcValid"] = (now.year() >= 2020);
  h["freeHeapKB"] = ESP.getFreeHeap() / 1024;
  h["uptimeMin"] = millis() / 60000;

#ifdef MODULE_IRRIGATION
  irrigation.appendStatus(root);
#endif
#ifdef MODULE_LIGHTING
  lighting.appendStatus(root);
#endif

  String out;
  serializeJson(root, out);
  return out;
}

// --- Remote server command executor ---
void applyRemoteCommand(const String& action, JsonObject params, const String& commandId, String& ackedIdsCsv, size_t& ackCount) {
#ifdef MODULE_IRRIGATION
  if (action == "zone/on") {
    int zoneId = params["id"] | -1;
    int dur = params["dur"] | 15;
    if (dur <= 0) dur = 15;
    int idx = irrigation.findZone((uint8_t)zoneId);
    if (idx >= 0 && irrigation.start(idx, (uint16_t)dur, "REMOTE")) {
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
    }
    return;
  }
  if (action == "zone/off") {
    int zoneId = params["id"] | -1;
    int idx = irrigation.findZone((uint8_t)zoneId);
    if (idx >= 0 && irrigation.stop(idx, "REMOTE")) {
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
    }
    return;
  }
  if (action == "zone/schedule") {
    int zoneId = params["id"] | -1;
    int hour = params["hour"] | 0;
    int minute = params["minute"] | 0;
    int dur = params["dur"] | 15;
    int idx = irrigation.findZone((uint8_t)zoneId);
    if (idx >= 0 && irrigation.setSchedule(idx, (uint8_t)hour, (uint8_t)minute, (uint16_t)dur)) {
      queueEvent("schedule", "set");
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
    }
    return;
  }
  if (action == "pump/on") {
    irrigation.clearPumpOverride();
    irrigation.setPump(true, "REMOTE");
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
  if (action == "pump/off") {
    irrigation.clearPumpOverride();
    for (uint8_t i = 0; i < irrigation.zoneCount(); i++) irrigation.stop(i, "REMOTE");
    irrigation.setPump(false, "REMOTE");
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
  if (action == "e") {
    irrigation.clearPumpOverride();
    for (uint8_t i = 0; i < irrigation.zoneCount(); i++) irrigation.stop(i, "REMOTE");
    irrigation.setPump(false, "REMOTE");
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
  if (action == "config/time") {
    int year = params["year"] | 2026;
    int month = params["month"] | 1;
    int day = params["day"] | 1;
    int hour = params["hour"] | 0;
    int minute = params["minute"] | 0;
    rtc.set(DateTime(year, month, day, hour, minute, 0));
    queueEvent("config", "time-set");
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
  if (action == "config/sync") {
    String url = params["url"] | String("");
    String apiKey = params["apiKey"] | String("");
    if (url.length()) {
      setServerUrl(url);
      setServerEnabled(true);
      if (apiKey.length() > 0) setApiKey(apiKey);
      queueEvent("config", "sync-set");
      syncTriggerNow();
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
    }
    return;
  }
#endif
#ifdef MODULE_LIGHTING
  if (action == "lighting/toggle") {
    int id = params["id"] | 0;
    if (lighting.toggleChannel((uint8_t)id)) {
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
    }
    return;
  }
  if (action == "lighting/all-on") {
    lighting.allOn();
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
  if (action == "lighting/all-off") {
    lighting.allOff();
    ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
    ackCount++;
    return;
  }
#endif
  if (action == "config/wifi") {
    const String ssid = params["ssid"] | String("");
    const String password = params["password"] | String("");
    if (ssid.length()) {
      wifi.save(ssid, password);
      queueEvent("config", "wifi-set");
      ackedIdsCsv += (ackedIdsCsv.length() ? "," : "") + commandId;
      ackCount++;
      syncTriggerNow();
    }
  }
}

void pollAndExecuteServerCommands() {
  static uint32_t lastPoll = 0;
  static uint32_t pollInterval = 3000;
  uint32_t now = millis();
  if ((now - lastPoll) < pollInterval) return;
  lastPoll = now;

  if (!getServerEnabled() || WiFi.status() != WL_CONNECTED || getServerUrl().length() == 0) {
    return;
  }

  String response;
  if (!pollServerCommands(response, 2500)) return;

  const size_t cap = 2048;
  DynamicJsonDocument doc(cap);
  if (deserializeJson(doc, response) != DeserializationError::Ok) return;
  if (!doc["ok"].as<bool>()) return;

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (!cmds || cmds.isNull() || cmds.size() == 0) return;

  String ackIds = "[";
  size_t ackCount = 0;

  for (JsonObject item : cmds) {
    const String action = item["action"] | String("");
    JsonObject params = item["params"] | JsonObject();
      const String commandId = item["id"] | String("");
      if (commandId.length() == 0 || action.length() == 0) continue;
      applyRemoteCommand(action, params, commandId, ackIds, ackCount);
  }

  if (ackCount > 0) {
    ackIds += "]";
    ackServerCommands(ackIds);
  }
}

void loop() {
  uint32_t now = millis();

  wifi.update();
  led.update();

#ifdef MODULE_IRRIGATION
  irrigation.update(now);
#endif

#ifdef MODULE_LIGHTING
  lighting.update(now);
#endif

  webApp.update();
  static String serverStatusCache;
  static uint32_t serverStatusCacheTs = 0;
  if (serverStatusCacheTs == 0 || (now - serverStatusCacheTs) >= 5000) {
    serverStatusCache = buildServerStatusJson();
    serverStatusCacheTs = now;
  }
  syncLoop(serverStatusCache);

  // Handle OTA updates
  ArduinoOTA.handle();
  pollAndExecuteServerCommands();

  handleCLI();
  delay(2);
}
