#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/Services.h"

#define LIGHTING_CHANNELS 3

struct LightingChannel {
  uint8_t id;
  uint8_t pin;
  bool enabled;
  bool state;        // on/off
  bool scheduleEnabled;
  uint8_t onHour, onMinute;
  uint8_t offHour, offMinute;
};

class LightingService {
public:
  LightingService(RtcManager& rtc, StatusLed& led);

  void begin();
  void update(uint32_t now);
  void appendStatus(JsonObject object) const;

  // Direct control
  bool setChannel(uint8_t id, bool on);
  bool toggleChannel(uint8_t id);
  bool channelState(uint8_t id) const;

  // Scenes
  void allOn();
  void allOff();
  bool anyOn() const;

  // Schedules
  bool setSchedule(uint8_t id, uint8_t onH, uint8_t onM, uint8_t offH, uint8_t offM);
  bool enableSchedule(uint8_t id);
  bool disableSchedule(uint8_t id);

  void save();
  void load();

  // Static pin mapping — matched to the PRD and your board
  // Channel 0: GPIO13 (rooms), Channel 1: GPIO15 (living), Channel 2: GPIO4 (kitchen)
  static constexpr uint8_t CHANNEL_PINS[LIGHTING_CHANNELS] = {13, 15, 4};

private:
  RtcManager& rtc;
  StatusLed& led;

  LightingChannel channels[LIGHTING_CHANNELS];

  void writePin(uint8_t pin, bool on);
  void checkSchedule(uint32_t now);
  int findChannel(uint8_t id) const;
};
