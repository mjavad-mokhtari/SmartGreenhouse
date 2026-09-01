#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/Services.h"

class LightingService {
public:
  LightingService(RtcManager& rtc, StatusLed& led) : rtc(rtc), led(led) {}
  void begin(); void update(uint32_t now); void appendStatus(JsonObject object) const; void set(bool on);
private:
  RtcManager& rtc; StatusLed& led; bool enabled = false; static constexpr uint8_t channelPin = 27;
};