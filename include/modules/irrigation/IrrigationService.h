#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/Services.h"

#define ZONE_COUNT 4
#define MAX_RUN_MIN 120
#define MAX_EVENTS 30

struct Zone {
  uint8_t id, pin;
  bool enabled;
  bool days[8];      // index 1..7 = Sat..Fri
  bool ranToday;
  bool running;
  uint8_t hour, minute;
  uint16_t duration;  // minutes
  uint32_t started;   // millis() when started
};

class IrrigationService {
public:
  IrrigationService(RtcManager& rtc, StatusLed& led, PreferencesStore& store);

  void begin();
  void update(uint32_t now);
  void appendStatus(JsonObject object) const;

  // Zone management
  int  findZone(uint8_t id) const;
  bool addZone(uint8_t id, uint8_t pin);
  bool start(int idx, uint16_t durationMin, const char* reason);
  bool stop(int idx, const char* reason);

  // Pump
  void setPump(bool on, const char* why);
  void reconcilePump();
  bool pumpIsOn() const { return pump; }
  bool pumpManualOverrideActive() const { return pumpManualOverride; }
  void clearPumpOverride() { pumpManualOverride = false; }

  // Schedules
  void checkSchedules(uint32_t now);
  String nextRun(int idx) const;
  bool setSchedule(int idx, uint8_t hour, uint8_t minute, uint16_t durationMin);
  bool enableZone(int idx);
  bool disableZone(int idx);

  // Persistence
  void save();
  void load();

  // Access
  uint8_t zoneCount() const { return zoneN; }
  const Zone& zoneAt(uint8_t i) const { return zones[i]; }
  bool zoneRunning(uint8_t i) const { return i < zoneN && zones[i].running; }
  bool isEnabled(uint8_t i) const { return i < zoneN && zones[i].enabled; }

  // GPIO safety
  static bool zonePinAllowed(uint8_t pin);
  static const uint8_t DEFAULT_PINS[ZONE_COUNT];

  // Relay pin mapping (active channels) — matched to your running board
  static constexpr int8_t RELAY_PINS[8] = {25, 26, -1, -1, -1, -1, -1, -1};  // 2-ch relay board: ch0=pump(GPIO25), ch1=zone1(GPIO26), rest=direct GPIO
  static constexpr uint8_t PUMP_RELAY_CHANNEL = 0; // Relay 1 / IN1 → GPIO25
  static constexpr uint8_t ZONE1_RELAY_CHANNEL = 1; // Relay 2 / IN2 → GPIO26

  // Soil sensor
  int soilRawValue() const { return soilRaw; }
  int soilPercent() const;

  // LED feedback — delegate to StatusLed
  void triggerLed();

private:
  RtcManager& rtc;
  StatusLed& led;
  PreferencesStore& store;

  Zone zones[ZONE_COUNT];
  uint8_t zoneN = 0;

  bool pump = false;
  bool pumpManualOverride = false;

  // Soil sensor
  int soilRaw = 0;
  uint32_t lastSoilRead = 0;
  static constexpr uint8_t soilPowerPin = 33;
  static constexpr uint8_t soilAdcPin   = 34;

  // Helpers
  bool relayAssigned(uint8_t channel) const;
  bool isRelayPin(uint8_t pin) const;
  void relayWrite(uint8_t channel, bool on);
  void zoneWrite(uint8_t pin, bool on);
  int8_t relayChannelForZonePin(uint8_t pin) const;
  bool zoneUsesRelayPin(uint8_t pin) const;
  void allRelaysOff();
  void initRelays();

  uint8_t day7(uint8_t dow) const; // Sunday=0 → 7-based weekday
  void readSoil(uint32_t now);
};
