#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <RTClib.h>
#include <Wire.h>
#include <esp_system.h>
#include "sync.h"

// --- سخت‌افزار و پین‌ها ---
static const uint8_t LED_PIN = 2;
static const uint8_t RELAY_PINS[8] = {13, 12, 14, 27, 26, 25, 33, 32};
static const uint8_t PUMP_PIN = 13; // زون ۱ (رله ۱) به عنوان پمپ اختصاصی

// --- وضعیت سیستم ---
RTC_DS3231 rtc;
WebServer web(80);
Preferences prefs;

bool apMode = false;
bool pumpState = false;
bool zoneStates[8] = {false, false, false, false, false, false, false, false};

// تنظیمات زمان‌بندی زون‌ها
struct ScheduleConfig {
  bool enabled;
  uint8_t startHour;
  uint8_t startMin;
  uint16_t durationSec;
  uint8_t daysMask; // بیت ۰ تا ۶ (شنبه تا جمعه)
};
ScheduleConfig schedules[8];

// تایمرها
uint32_t zoneEndMillis[8] = {0};

void blinkLed(int times = 3) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(70);
    digitalWrite(LED_PIN, LOW);
    delay(70);
  }
  digitalWrite(LED_PIN, pumpState ? HIGH : LOW);
}

void setPump(bool state) {
  if (pumpState != state) {
    pumpState = state;
    digitalWrite(PUMP_PIN, pumpState ? LOW : HIGH); // رله Active LOW
    queueEvent("pump", pumpState ? "on" : "off");
    blinkLed(3);
  }
}

void reconcilePump() {
  bool anyZoneActive = false;
  // زون‌های ۲ به بعد (ایندکس‌های ۱ تا ۷)
  for (int i = 1; i < 8; i++) {
    if (zoneStates[i]) {
      anyZoneActive = true;
      break;
    }
  }
  // اگر زون ۱ دستی فعال باشد یا هر زون دیگری باز باشد، پمپ روشن می‌شود
  if (zoneStates[0] || anyZoneActive) {
    setPump(true);
  } else {
    setPump(false);
  }
}

void setZone(int idx, bool state, uint16_t durationSec = 0) {
  if (idx < 0 || idx >= 8) return;
  zoneStates[idx] = state;
  digitalWrite(RELAY_PINS[idx], state ? LOW : HIGH); // Active LOW
  
  if (state && durationSec > 0) {
    zoneEndMillis[idx] = millis() + (durationSec * 1000);
  } else if (!state) {
    zoneEndMillis[idx] = 0;
  }
  
  queueEvent("zone", state ? "on" : "off");
  reconcilePump();
  blinkLed(3);
}

void stopAllZones() {
  for (int i = 0; i < 8; i++) {
    zoneStates[i] = false;
    zoneEndMillis[i] = 0;
    digitalWrite(RELAY_PINS[i], HIGH); // خاموش (Active LOW)
  }
  setPump(false);
  queueEvent("system", "stop_all");
}

void loadSettings() {
  prefs.begin("irrigation", true);
  for (int i = 0; i < 8; i++) {
    String p = "z" + String(i);
    schedules[i].enabled = prefs.getBool((p + "_en").c_str(), false);
    schedules[i].startHour = prefs.getUChar((p + "_sh").c_str(), 8);
    schedules[i].startMin = prefs.getUChar((p + "_sm").c_str(), 0);
    schedules[i].durationSec = prefs.getUShort((p + "_du").c_str(), 300);
    schedules[i].daysMask = prefs.getUChar((p + "_dm").c_str(), 0x7F);
  }
  prefs.end();
}

void saveSchedule(int idx) {
  if (idx < 0 || idx >= 8) return;
  prefs.begin("irrigation", false);
  String p = "z" + String(idx);
  prefs.putBool((p + "_en").c_str(), schedules[idx].enabled);
  prefs.putUChar((p + "_sh").c_str(), schedules[idx].startHour);
  prefs.putUChar((p + "_sm").c_str(), schedules[idx].startMin);
  prefs.putUShort((p + "_du").c_str(), schedules[idx].durationSec);
  prefs.putUChar((p + "_dm").c_str(), schedules[idx].daysMask);
  prefs.end();
}

// --- توابع کمکی وب و API ---
void sendJson(const String& body) {
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.send(200, "application/json", body);
}

String getSystemStatusJson() {
  DateTime now = rtc.now();
  String out = "{";
  out += "\"uptime\":" + String(millis()) + ",";
  out += "\"wifi\":{\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  out += "\"ip\":\"" + (apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\",";
  out += "\"apMode\":" + String(apMode ? "true" : "false") + "},";
  out += "\"rtc\":{\"valid\":" + String(now.isValid() ? "true" : "false") + ",";
  out += "\"time\":\"" + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second()) + "\"},";
  out += "\"pump\":" + String(pumpState ? "true" : "false") + ",";
  out += "\"zones\":[";
  for (int i = 0; i < 8; i++) {
    if (i > 0) out += ",";
    uint32_t rem = 0;
    if (zoneStates[i] && zoneEndMillis[i] > millis()) {
      rem = (zoneEndMillis[i] - millis()) / 1000;
    }
    out += "{\"id\":" + String(i + 1) + ",\"state\":" + String(zoneStates[i] ? "true" : "false") + ",\"rem\":" + String(rem) + "}";
  }
  out += "]}";
  return out;
}

void registerRoutes() {
  web.on("/health", HTTP_GET, []() {
    sendJson("{\"status\":\"ok\",\"freeHeap\":" + String(ESP.getFreeHeap()) + "}");
  });

  web.on("/api/status", HTTP_GET, []() {
    sendJson(getSystemStatusJson());
  });

  web.on("/api/zone", HTTP_GET, []() {
    if (web.hasArg("id") && web.hasArg("state")) {
      int id = web.arg("id").toInt() - 1;
      bool state = web.arg("state") == "1" || web.arg("state") == "true";
      int dur = web.hasArg("dur") ? web.arg("dur").toInt() : 0;
      setZone(id, state, dur);
      sendJson("{\"result\":\"success\"}");
    } else {
      web.send(400, "text/plain", "Bad Arguments");
    }
  });

  web.on("/api/stop", HTTP_GET, []() {
    stopAllZones();
    sendJson("{\"result\":\"stopped_all\"}");
  });

  // مسیرهای ماژول Sync
  web.on("/api/sync/now", HTTP_GET, []() {
    syncTriggerNow();
    sendJson(syncStatusJson());
  });

  web.on("/api/server/config", HTTP_GET, []() {
    if (web.hasArg("url")) setServerUrl(web.arg("url"));
    if (web.hasArg("en")) setServerEnabled(web.arg("en") == "1" || web.arg("en") == "true");
    sendJson(syncStatusJson());
  });
}

// --- CLI ترمینال سریال ---
void handleSerialCLI() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "status") {
      Serial.println(getSystemStatusJson());
    } else if (cmd.startsWith("zone ")) {
      int id = cmd.substring(5, 6).toInt() - 1;
      int state = cmd.substring(7).toInt();
      setZone(id, state == 1);
      Serial.printf("Zone %d set to %d\n", id + 1, state);
    } else if (cmd == "stop") {
      stopAllZones();
      Serial.println("All zones stopped.");
    } else if (cmd == "sync") {
      syncTriggerNow();
      Serial.println("Sync triggered.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH); // رله خاموش (Active LOW)
  }

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC DS3231 not found!");
  }

  loadSettings();

  // تلاش برای اتصال به شبکه خانگی
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  
  uint32_t startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(200);
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to Wi-Fi. IP: ");
    Serial.println(WiFi.localIP());
    apMode = false;
  } else {
    Serial.println("Wi-Fi STA failed. Starting AP mode fallback...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SmartHome-ESP32", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    apMode = true;
  }

  // فعال‌سازی اتصال مجدد و بارگذاری ماژول Sync
  WiFi.setAutoReconnect(true);
  syncLoadConfig();

  registerRoutes();
  web.begin();
  Serial.println("Web server started.");
  blinkLed(3);
}

void loop() {
  // ۱. بررسی اتصال مجدد شبکه (Non-blocking)
  static uint32_t lastReconnect = 0;
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    if (millis() - lastReconnect >= 30000) {
      lastReconnect = millis();
      WiFi.reconnect();
    }
  }

  // ۲. مدیریت همگام‌سازی سرور
  syncLoop();

  // ۳. رسیدگی به کلاینت‌های وب و کنسول CLI
  web.handleClient();
  handleSerialCLI();

  // ۴. بررسی تایمر خاموشی خودکار زون‌ها
  uint32_t now = millis();
  for (int i = 0; i < 8; i++) {
    if (zoneStates[i] && zoneEndMillis[i] > 0 && now >= zoneEndMillis[i]) {
      setZone(i, false);
    }
  }
}
