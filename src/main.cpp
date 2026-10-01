#include <Arduino.h>
#include <esp_system.h>
#include <sys/time.h>
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

static void syncSystemClockFromRtc() {
  if (!rtc.isAvailable()) return;
  const DateTime now = rtc.now();
  if (now.year() < 2020) return;
  timeval tv{};
  tv.tv_sec = now.unixtime();
  settimeofday(&tv, nullptr);
}

void setup() {
  Serial.begin(115200);

  led.begin();
  rtc.begin();
  syncSystemClockFromRtc();

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
  syncLoadDeviceId();
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
String remoteCommandDoneIds() {
  Preferences prefs;
  prefs.begin("cmdids", true);
  String ids = prefs.getString("done", "|");
  prefs.end();
  return ids;
}

bool remoteCommandWasApplied(const String& commandId) {
  const String ids = remoteCommandDoneIds();
  return ids.indexOf("|" + commandId + "|") >= 0;
}

void rememberRemoteCommandApplied(const String& commandId) {
  String ids = remoteCommandDoneIds();
  const String token = "|" + commandId + "|";
  if (ids.indexOf(token) >= 0) return;
  if (!ids.endsWith("|")) ids += "|";
  ids += commandId + "|";
  while (ids.length() > 320) {
    const int next = ids.indexOf('|', 1);
    if (next < 0) { ids = "|"; break; }
    ids.remove(1, next);
    if (ids.startsWith("||")) ids.remove(1, 1);
  }
  Preferences prefs;
  prefs.begin("cmdids", false);
  prefs.putString("done", ids);
  prefs.end();
}

void applyRemoteCommand(const String& action, JsonObject params, String& result, String& detail) {
  result = "rejected";
  detail = "فرمان در این firmware پشتیبانی نمی‌شود";
#ifdef MODULE_IRRIGATION
  if (action == "zone/on") {
    int zoneId = params["id"] | -1;
    int dur = params["dur"] | 15;
    int idx = irrigation.findZone((uint8_t)zoneId);
    if (idx >= 0 && dur > 0 && irrigation.start(idx, (uint16_t)dur, "REMOTE")) { result = "applied"; detail = "آبیاری اجرا شد"; }
    else detail = "زون یا مدت آبیاری نامعتبر است";
    return;
  }
  if (action == "zone/off") {
    int zoneId = params["id"] | -1;
    int idx = irrigation.findZone((uint8_t)zoneId);
    if (idx >= 0 && irrigation.stop(idx, "REMOTE")) { result = "applied"; detail = "زون متوقف شد"; }
    else detail = "زون پیدا نشد";
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
      syncTriggerNow();
      result = "applied"; detail = "زمان‌بندی ذخیره شد";
    } else {
      detail = "زون یا مقادیر زمان‌بندی نامعتبر است";
    }
    return;
  }
  if (action == "pump/on") {
    irrigation.clearPumpOverride();
    irrigation.setPump(true, "REMOTE");
    result = "applied"; detail = "پمپ روشن شد";
    return;
  }
  if (action == "pump/off" || action == "e") {
    irrigation.clearPumpOverride();
    for (uint8_t i = 0; i < irrigation.zoneCount(); i++) irrigation.stop(i, "REMOTE");
    irrigation.setPump(false, "REMOTE");
#ifdef MODULE_LIGHTING
    if (action == "e") lighting.allOff();
#endif
    result = "applied"; detail = action == "e" ? "توقف اضطراری آبیاری اجرا شد" : "پمپ و زون‌ها متوقف شدند";
    return;
  }
#endif

  if (action == "e") {
#ifdef MODULE_LIGHTING
    lighting.allOff();
#endif
    result = "applied"; detail = "توقف اضطراری اجرا شد";
    return;
  }
  if (action == "config/time") {
    int year = params["year"] | 2026;
    int month = params["month"] | 1;
    int day = params["day"] | 1;
    int hour = params["hour"] | 0;
    int minute = params["minute"] | 0;
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      detail = "تاریخ یا ساعت نامعتبر است";
      return;
    }
    rtc.set(DateTime(year, month, day, hour, minute, 0));
    syncSystemClockFromRtc();
    queueEvent("config", "time-set");
    result = "applied"; detail = "ساعت تنظیم شد";
    return;
  }
  if (action == "config/sync") {
    String url = params["url"] | String("");
    String apiKey = params["apiKey"] | String("");
    if (url.length()) {
      if (url.startsWith("https://") && !probeServerUrl(url)) {
        detail = "اتصال امن به سرور تایید نشد؛ نشانی قبلی حفظ شد";
        return;
      }
      setServerUrl(url);
      setServerEnabled(true);
      if (apiKey.length() > 0) setApiKey(apiKey);
      queueEvent("config", "sync-set");
      syncTriggerNow();
      result = "applied"; detail = "همگام‌سازی تنظیم شد";
    } else {
      detail = "نشانی سرور خالی است";
    }
    return;
  }
#ifdef MODULE_LIGHTING
  if (action == "lighting/toggle") {
    int id = params["id"] | 0;
    if (lighting.toggleChannel((uint8_t)id)) {
      result = "applied"; detail = "وضعیت کانال تغییر کرد";
    } else {
      detail = "کانال پیدا نشد";
    }
    return;
  }
  if (action == "lighting/set") {
    int id = params["id"] | 0;
    if (!params["state"].is<bool>()) { detail = "وضعیت کانال نامعتبر است"; return; }
    if (lighting.setChannel((uint8_t)id, params["state"].as<bool>())) {
      result = "applied"; detail = "وضعیت کانال تنظیم شد";
    } else detail = "کانال پیدا نشد";
    return;
  }
  if (action == "lighting/all-on") {
    lighting.allOn();
    result = "applied"; detail = "همه کانال‌ها روشن شدند";
    return;
  }
  if (action == "lighting/all-off") {
    lighting.allOff();
    result = "applied"; detail = "همه کانال‌ها خاموش شدند";
    return;
  }
#endif
  if (action == "config/wifi") {
    const String ssid = params["ssid"] | String("");
    const String password = params["password"] | String("");
    if (ssid.length()) {
      wifi.save(ssid, password);
      queueEvent("config", "wifi-set");
      result = "applied"; detail = "تنظیم Wi-Fi ذخیره شد";
      syncTriggerNow();
    } else detail = "نام شبکه خالی است";
    return;
  }
}

bool pollAndExecuteServerCommands() {
  static uint32_t lastPoll = 0;
  static uint32_t pollInterval = 1000;
  uint32_t now = millis();
  if ((now - lastPoll) < pollInterval) return false;
  lastPoll = now;

  if (!getServerEnabled() || WiFi.status() != WL_CONNECTED || getServerUrl().length() == 0) {
    return false;
  }

  String response;
  if (!pollServerCommands(response, 1200)) return false;

  const size_t cap = 2048;
  DynamicJsonDocument doc(cap);
  if (deserializeJson(doc, response) != DeserializationError::Ok) return false;
  if (!doc["ok"].as<bool>()) return false;

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (!cmds || cmds.isNull() || cmds.size() == 0) return false;

  JsonObject item = cmds[0];
  const String action = item["action"] | String("");
  const String commandId = item["id"] | String("");
  if (commandId.length() == 0 || action.length() == 0) return false;

  String result = "rejected";
  String detail;
  const JsonVariant remaining = item["remainingMs"];
  if (!remaining.isNull() && remaining.as<uint32_t>() == 0) {
    result = "expired";
    detail = "مهلت فرمان هنگام دریافت تمام شد";
  } else if (remoteCommandWasApplied(commandId)) {
    result = "applied";
    detail = "فرمان قبلاً اجرا شده بود؛ اجرای دوباره انجام نشد";
  } else {
    JsonObject params = item["params"].as<JsonObject>();
    applyRemoteCommand(action, params, result, detail);
    if (result == "applied") rememberRemoteCommandApplied(commandId);
  }
  ackServerCommand(commandId, result, detail);
  if (result == "applied") {
    syncTriggerNow();
    return true;
  }
  return false;
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
  const bool remoteCommandApplied = pollAndExecuteServerCommands();
  if (remoteCommandApplied || serverStatusCacheTs == 0 || (now - serverStatusCacheTs) >= 5000) {
    serverStatusCache = buildServerStatusJson();
    serverStatusCacheTs = millis();
  }

  // Keep command retrieval ahead of the slower telemetry retry path; refresh status immediately after changes.
  syncLoop(serverStatusCache);

  // Handle OTA updates
  ArduinoOTA.handle();

  handleCLI();
  delay(2);
}
