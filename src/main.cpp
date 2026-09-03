#include <Arduino.h>
#include <esp_system.h>
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
  syncLoop();

  handleCLI();
  delay(2);
}
