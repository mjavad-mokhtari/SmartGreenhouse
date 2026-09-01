#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <RTClib.h>
#include <WiFi.h>
#include <Wire.h>

class RtcManager {
public:
  void begin() { Wire.begin(21, 22); available = rtc.begin(); if (available && rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); }
  void update() {}
  DateTime now() { return rtc.now(); }
  bool isAvailable() const { return available; }
  void set(const DateTime& value) { if (available) rtc.adjust(value); }
private:
  RTC_DS3231 rtc;
  bool available = false;
};

class PreferencesStore {
public:
  explicit PreferencesStore(const char* name) : name(name) {}
  String get(const char* key, const String& fallback = String()) { Preferences p; p.begin(name, true); String value = p.getString(key, fallback); p.end(); return value; }
  void put(const char* key, const String& value) { Preferences p; p.begin(name, false); p.putString(key, value); p.end(); }
private:
  const char* name;
};

class WifiManager {
public:
  void begin() { ssid = store.get("ssid"); password = store.get("password"); if (ssid.length()) { WiFi.mode(WIFI_STA); WiFi.begin(ssid, password); } else startAccessPoint(); }
  void update() { if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 15000) { lastAttempt = millis(); WiFi.reconnect(); } if (!apMode && WiFi.status() != WL_CONNECTED && millis() > 12000 && !hasTriedAp) startAccessPoint(); }
  void save(const String& newSsid, const String& newPassword) { ssid = newSsid; password = newPassword; store.put("ssid", ssid); store.put("password", password); }
  bool connected() const { return apMode || WiFi.status() == WL_CONNECTED; }
  String ip() const { return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }
  String network() const { return apMode ? "SGH-Setup" : ssid; }
private:
  void startAccessPoint() { apMode = true; hasTriedAp = true; WiFi.mode(WIFI_AP); WiFi.softAP("SGH-Setup", "irrigation"); }
  PreferencesStore store{"system"};
  String ssid, password;
  bool apMode = false, hasTriedAp = false;
  uint32_t lastAttempt = 0;
};

class StatusLed {
public:
  explicit StatusLed(uint8_t pin) : pin(pin) {}
  void begin() { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  void update() { if (pulseUntil && (int32_t)(millis() - pulseUntil) >= 0) { digitalWrite(pin, LOW); pulseUntil = 0; } }
  void pulse(uint16_t duration = 80) { digitalWrite(pin, HIGH); pulseUntil = millis() + duration; }
private:
  uint8_t pin;
  uint32_t pulseUntil = 0;
};