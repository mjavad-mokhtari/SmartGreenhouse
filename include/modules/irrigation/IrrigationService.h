#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/Services.h"

class IrrigationService {
public:
  IrrigationService(RtcManager& rtc, StatusLed& led) : rtc(rtc), led(led) {}
  void begin();
  void update(uint32_t now);
  void appendStatus(JsonObject object) const;
  void setPump(bool on);
private:
  RtcManager& rtc; StatusLed& led; bool pump = false; bool zoneOn = false; int soilRaw = 0; uint32_t lastSoilRead = 0;
  static constexpr uint8_t pumpPin = 25, zonePin = 26, soilPowerPin = 33, soilAdcPin = 34;
};