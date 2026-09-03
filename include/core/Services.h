#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <RTClib.h>
#include <WiFi.h>
#include <Wire.h>

class RtcManager {
public:
  void begin() {
    Wire.begin(21, 22);
    available = rtc.begin();
    if (available && rtc.lostPower())
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
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
  String get(const char* key, const String& fallback = String()) {
    Preferences p; p.begin(name, true);
    String value = p.getString(key, fallback); p.end();
    return value;
  }
  void put(const char* key, const String& value) {
    Preferences p; p.begin(name, false);
    p.putString(key, value); p.end();
  }
  const char* name;
};

class WifiManager {
public:
  explicit WifiManager(PreferencesStore& store) : store(store) {}
  void begin() {
    ssid = store.get("ssid");
    password = store.get("pass");
    if (ssid.length()) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), password.c_str());
      uint32_t t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) { delay(250); }
    }
    if (WiFi.status() != WL_CONNECTED) startAccessPoint();
  }
  void update() {
    if (!apMode && WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 30000) {
      lastAttempt = millis();
      WiFi.reconnect();
    }
  }
  void save(const String& newSsid, const String& newPassword) {
    ssid = newSsid; password = newPassword;
    store.put("ssid", ssid); store.put("pass", password);
  }
  bool connected() const { return apMode || WiFi.status() == WL_CONNECTED; }
  String ip() const { return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }
  String network() const { return apMode ? "SGH-Setup" : ssid; }
  bool isAccessPoint() const { return apMode; }
private:
  void startAccessPoint() {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SGH-Setup", "irrigation");
  }
  PreferencesStore& store;
  String ssid, password;
  bool apMode = false;
  uint32_t lastAttempt = 0;
};

class StatusLed {
public:
  explicit StatusLed(uint8_t pin) : pin(pin) {}
  void begin() { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  void update() {
    if (!blinking) {
      if (pulseUntil && (int32_t)(millis() - pulseUntil) >= 0) { digitalWrite(pin, LOW); pulseUntil = 0; }
      return;
    }
    uint32_t now = millis();
    if ((int32_t)(now - blinkNext) >= 0) {
      blinkToggless--;
      if (blinkToggless > 0) {
        blinkState = !blinkState;
        digitalWrite(pin, blinkState ? HIGH : LOW);
        blinkNext = now + blinkInterval;
      } else {
        blinking = false;
        blinkState = steadyOn;
        digitalWrite(pin, steadyOn ? HIGH : LOW);
      }
    }
  }

  void blink(uint16_t count = 3, uint16_t intervalMs = 100) {
    // count * 2 toggles (on+off per cycle)
    blinkToggless = count * 2;
    blinkInterval = intervalMs;
    blinkState = false;
    blinking = true;
    blinkNext = 0;
  }

  void steadyState(bool on) { steadyOn = on; }

private:
  uint8_t pin;
  uint32_t pulseUntil =0;
  bool blinking = false;
  uint16_t blinkToggless =0;
  uint16_t blinkInterval = 100;
  uint32_t blinkNext =0;
  bool blinkState = false;
  bool steadyOn = false;
};
