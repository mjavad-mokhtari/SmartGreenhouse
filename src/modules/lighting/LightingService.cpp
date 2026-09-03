#include "modules/lighting/LightingService.h"
#include "sync.h"

constexpr uint8_t LightingService::CHANNEL_PINS[LIGHTING_CHANNELS];

LightingService::LightingService(RtcManager& rtc, StatusLed& led)
  : rtc(rtc), led(led) {}

int LightingService::findChannel(uint8_t id) const {
  for (int i = 0; i < LIGHTING_CHANNELS; i++)
    if (channels[i].id == id) return i;
  return -1;
}

void LightingService::writePin(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH); // Active-Low
}

void LightingService::begin() {
  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    channels[i].id = i + 1;
    channels[i].pin = CHANNEL_PINS[i];
    channels[i].enabled = true;
    channels[i].state = false;
    channels[i].scheduleEnabled = false;
    channels[i].onHour = 0;
    channels[i].onMinute = 0;
    channels[i].offHour = 0;
    channels[i].offMinute = 0;

    pinMode(channels[i].pin, OUTPUT);
    digitalWrite(channels[i].pin, HIGH); // off (Active-Low)
  }
  load();
}

void LightingService::update(uint32_t now) {
  checkSchedule(now);
}

bool LightingService::setChannel(uint8_t id, bool on) {
  int idx = findChannel(id);
  if (idx < 0) return false;
  channels[idx].state = on;
  writePin(channels[idx].pin, on);
  led.blink(1, 80);
  queueEvent("light", on ? "on" : "off");
  return true;
}

bool LightingService::toggleChannel(uint8_t id) {
  int idx = findChannel(id);
  if (idx < 0) return false;
  channels[idx].state = !channels[idx].state;
  writePin(channels[idx].pin, channels[idx].state);
  led.blink(1, 80);
  queueEvent("light", channels[idx].state ? "on" : "off");
  return true;
}

bool LightingService::channelState(uint8_t id) const {
  int idx = findChannel(id);
  return idx >= 0 && channels[idx].state;
}

void LightingService::allOn() {
  // Stagger turn-ons to reduce inrush current
  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    channels[i].state = true;
    writePin(channels[i].pin, true);
    delay(20);
  }
  led.blink(1, 80);
  queueEvent("light", "all-on");
}

void LightingService::allOff() {
  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    channels[i].state = false;
    writePin(channels[i].pin, false);
  }
  led.blink(1, 80);
  queueEvent("light", "all-off");
}

bool LightingService::anyOn() const {
  for (int i = 0; i < LIGHTING_CHANNELS; i++)
    if (channels[i].state) return true;
  return false;
}

bool LightingService::setSchedule(uint8_t id, uint8_t onH, uint8_t onM, uint8_t offH, uint8_t offM) {
  int idx = findChannel(id);
  if (idx < 0) return false;
  channels[idx].onHour = onH;
  channels[idx].onMinute = onM;
  channels[idx].offHour = offH;
  channels[idx].offMinute = offM;
  channels[idx].scheduleEnabled = true;
  save();
  return true;
}

bool LightingService::enableSchedule(uint8_t id) {
  int idx = findChannel(id);
  if (idx < 0) return false;
  channels[idx].scheduleEnabled = true;
  save();
  return true;
}

bool LightingService::disableSchedule(uint8_t id) {
  int idx = findChannel(id);
  if (idx < 0) return false;
  channels[idx].scheduleEnabled = false;
  save();
  return true;
}

void LightingService::checkSchedule(uint32_t now) {
  DateTime n = rtc.now();
  int currentMinute = n.hour() * 60 + n.minute();

  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    LightingChannel& c = channels[i];
    if (!c.enabled || !c.scheduleEnabled) continue;

    int onMin = c.onHour * 60 + c.onMinute;
    int offMin = c.offHour * 60 + c.offMinute;

    if (onMin == offMin) continue; // no valid schedule

    if (onMin < offMin) {
      // Simple schedule within same day: onMin → offMin
      if (currentMinute >= onMin && currentMinute < offMin) {
        if (!c.state) setChannel(c.id, true);
      } else {
        if (c.state) setChannel(c.id, false);
      }
    } else {
      // Schedule crosses midnight: onMin → midnight → offMin
      if (currentMinute >= onMin || currentMinute < offMin) {
        if (!c.state) setChannel(c.id, true);
      } else {
        if (c.state) setChannel(c.id, false);
      }
    }
  }
}

void LightingService::save() {
  Preferences p;
  p.begin("lighting", false);
  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    String base = "ch" + String(i) + "_";
    const LightingChannel& c = channels[i];
    p.putBool((base + "en").c_str(), c.enabled);
    p.putBool((base + "sch").c_str(), c.scheduleEnabled);
    p.putUChar((base + "onh").c_str(), c.onHour);
    p.putUChar((base + "onm").c_str(), c.onMinute);
    p.putUChar((base + "offh").c_str(), c.offHour);
    p.putUChar((base + "offm").c_str(), c.offMinute);
  }
  p.end();
}

void LightingService::load() {
  Preferences p;
  p.begin("lighting", true);
  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    String base = "ch" + String(i) + "_";
    channels[i].enabled = p.getBool((base + "en").c_str(), true);
    channels[i].scheduleEnabled = p.getBool((base + "sch").c_str(), false);
    channels[i].onHour = p.getUChar((base + "onh").c_str(), 0);
    channels[i].onMinute = p.getUChar((base + "onm").c_str(), 0);
    channels[i].offHour = p.getUChar((base + "offh").c_str(), 0);
    channels[i].offMinute = p.getUChar((base + "offm").c_str(), 0);
  }
  p.end();
}

void LightingService::appendStatus(JsonObject object) const {
  JsonObject l = object.createNestedObject("lighting");
  l["channels"] = LIGHTING_CHANNELS;
  JsonArray ca = l.createNestedArray("state");

  for (int i = 0; i < LIGHTING_CHANNELS; i++) {
    JsonObject ch = ca.createNestedObject();
    const LightingChannel& c = channels[i];
    ch["id"] = c.id;
    ch["pin"] = c.pin;
    ch["enabled"] = c.enabled;
    ch["state"] = c.state;
    ch["scheduleEnabled"] = c.scheduleEnabled;
    ch["onTime"] = String(c.onHour) + ":" + (c.onMinute < 10 ? "0" : "") + String(c.onMinute);
    ch["offTime"] = String(c.offHour) + ":" + (c.offMinute < 10 ? "0" : "") + String(c.offMinute);
  }
}
