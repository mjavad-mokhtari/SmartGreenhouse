#include "modules/irrigation/IrrigationService.h"
#include <esp_system.h>

// --- static pin defaults ---
const uint8_t IrrigationService::DEFAULT_PINS[ZONE_COUNT] = {23, 25, 26, 27};

// --- constructor ---
IrrigationService::IrrigationService(RtcManager& rtc, StatusLed& led, PreferencesStore& store)
  : rtc(rtc), led(led), store(store) {}

// --- day7: convert Arduino dayOfTheWeek (Sun=0) to index 1..7 (Sat=1..Fri=7) ---
uint8_t IrrigationService::day7(uint8_t dow) const {
  // In Arduino: Sunday=0, Monday=1, ..., Saturday=6
  // We want: Saturday=1, Sunday=2, ..., Friday=7
  return (dow == 0) ? 2 : ((dow + 2) > 7 ? (dow + 2 - 7) : (dow + 2));
}

// --- relay helpers ---
bool IrrigationService::relayAssigned(uint8_t channel) const {
  return channel < 8 && RELAY_PINS[channel] >= 0;
}
bool IrrigationService::isRelayPin(uint8_t pin) const {
  for (uint8_t i = 0; i < 8; i++)
    if (relayAssigned(i) && (uint8_t)RELAY_PINS[i] == pin) return true;
  return false;
}
void IrrigationService::relayWrite(uint8_t channel, bool on) {
  if (relayAssigned(channel))
    digitalWrite((uint8_t)RELAY_PINS[channel], on ? LOW : HIGH); // Active-Low
}
int8_t IrrigationService::relayChannelForZonePin(uint8_t pin) const {
  for (uint8_t i = 0; i < 8; i++)
    if (relayAssigned(i) && (uint8_t)RELAY_PINS[i] == pin)
      return (int8_t)i;
  return -1;
}
bool IrrigationService::zoneUsesRelayPin(uint8_t pin) const {
  return relayChannelForZonePin(pin) >= 0;
}

void IrrigationService::zoneWrite(uint8_t pin, bool on) {
  int8_t channel = relayChannelForZonePin(pin);
  if (channel == PUMP_RELAY_CHANNEL) {
    Serial.printf("WARNING: zone on GPIO%d conflicts with pump relay; output blocked\n", pin);
    return;
  }
  if (channel >= 0) { relayWrite((uint8_t)channel, on); return; }
  if (pin == 2) {
    Serial.println("WARNING: zone output blocked because GPIO2 is LED feedback");
    return;
  }
  digitalWrite(pin, on ? HIGH : LOW);
}

void IrrigationService::allRelaysOff() {
  for (uint8_t i = 0; i < 8; i++) relayWrite(i, false);
  pump = false;
}

void IrrigationService::initRelays() {
  for (uint8_t i = 0; i < 8; i++) {
    if (relayAssigned(i)) {
      digitalWrite((uint8_t)RELAY_PINS[i], HIGH); // latch OFF before output
      pinMode((uint8_t)RELAY_PINS[i], OUTPUT);
      digitalWrite((uint8_t)RELAY_PINS[i], HIGH);
    }
  }
}

// --- GPIO safety ---
bool IrrigationService::zonePinAllowed(uint8_t pin) {
  if (pin == 25) { Serial.println("ERROR: GPIO25 reserved for pump"); return false; }
  if (pin == 2)  { Serial.println("ERROR: GPIO2 reserved for LED");   return false; }
  return true;
}

// --- soil sensor ---
void IrrigationService::readSoil(uint32_t now) {
  if (now - lastSoilRead < 5000) return;
  lastSoilRead = now;
  digitalWrite(soilPowerPin, HIGH);
  delay(10);
  soilRaw = analogRead(soilAdcPin);
  digitalWrite(soilPowerPin, LOW);
}

int IrrigationService::soilPercent() const {
  return constrain(map(soilRaw, 3000, 1400, 0, 100), 0, 100);
}

// --- pump ---
void IrrigationService::setPump(bool on, const char* why) {
  if (!relayAssigned(PUMP_RELAY_CHANNEL)) { pump = false; return; }
  relayWrite(PUMP_RELAY_CHANNEL, on);
  if (pump != on) { pump = on; triggerLed(); }
}

void IrrigationService::reconcilePump() {
  bool any = false;
  for (uint8_t i = 0; i < zoneN; i++)
    if (zones[i].running) { any = true; break; }
  if (!pumpManualOverride) setPump(any, "Automatic zone demand");
}

void IrrigationService::triggerLed() {
  led.blink(3, 100);
  // After blinks, steady state: ON if pump or any zone running
  bool steady = pump;
  for (uint8_t i = 0; i < zoneN; i++)
    if (zones[i].running) { steady = true; break; }
  led.steadyState(steady);
}

// --- zone management ---
int IrrigationService::findZone(uint8_t id) const {
  for (uint8_t i = 0; i < zoneN; i++)
    if (zones[i].id == id) return (int)i;
  return -1;
}

bool IrrigationService::addZone(uint8_t id, uint8_t pin) {
  if (findZone(id) >= 0) return false;
  if (zoneN >= ZONE_COUNT) return false;
  if (!zonePinAllowed(pin)) return false;
  // Pin must not conflict with a relay that's already used by another zone
  for (uint8_t i = 0; i < zoneN; i++)
    if (zones[i].pin == pin) return false;

  zones[zoneN] = {};
  zones[zoneN].id = id;
  zones[zoneN].pin = pin;
  zones[zoneN].enabled = false;
  for (int d = 1; d <= 7; d++) zones[zoneN].days[d] = true;

  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  zoneN++;
  save();
  return true;
}

bool IrrigationService::start(int idx, uint16_t durationMin, const char* reason) {
  if (idx < 0 || idx >= (int)zoneN) return false;
  if (durationMin > MAX_RUN_MIN) durationMin = MAX_RUN_MIN;
  if (durationMin == 0) return false;

  Zone& z = zones[idx];
  pumpManualOverride = false; // clear manual override on zone start
  z.running = true;
  z.duration = durationMin;
  z.started = millis();
  zoneWrite(z.pin, true);
  reconcilePump();
  triggerLed();
  return true;
}

bool IrrigationService::stop(int idx, const char* reason) {
  if (idx < 0 || idx >= (int)zoneN) return false;
  Zone& z = zones[idx];
  if (!z.running) return false;
  z.running = false;
  z.started = 0;
  zoneWrite(z.pin, false);
  reconcilePump();
  triggerLed();
  return true;
}

// --- schedules ---
bool IrrigationService::setSchedule(int idx, uint8_t hour, uint8_t minute, uint16_t durationMin) {
  if (idx < 0 || idx >= (int)zoneN) return false;
  if (hour > 23 || minute > 59) return false;
  if (durationMin == 0 || durationMin > MAX_RUN_MIN) return false;

  Zone& z = zones[idx];
  z.hour = hour;
  z.minute = minute;
  z.duration = durationMin;
  for (int d = 1; d <= 7; d++) z.days[d] = true;
  save();
  return true;
}

bool IrrigationService::enableZone(int idx) {
  if (idx < 0 || idx >= (int)zoneN) return false;
  zones[idx].enabled = true;
  save();
  return true;
}

bool IrrigationService::disableZone(int idx) {
  if (idx < 0 || idx >= (int)zoneN) return false;
  zones[idx].enabled = false;
  stop(idx, "Disabled");
  save();
  return true;
}

String IrrigationService::nextRun(int idx) const {
  if (idx < 0 || idx >= (int)zoneN) return "N/A";
  const Zone& z = zones[idx];
  if (!z.enabled) return "N/A";
  if (z.hour == 0 && z.minute == 0) return "None";
  char buf[14];
  snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", 0, 0, z.hour, z.minute);
  return String(buf);
}

void IrrigationService::checkSchedules(uint32_t now) {
  DateTime n = rtc.now();
  static int lastDay = -1;

  if (n.day() != lastDay) {
    lastDay = n.day();
    for (uint8_t i = 0; i < zoneN; i++) zones[i].ranToday = false;
  }

  // Check time-based completion
  for (uint8_t i = 0; i < zoneN; i++) {
    if (zones[i].running) {
      uint32_t elapsedMs = now - zones[i].started;
      if (elapsedMs >= (uint32_t)zones[i].duration * 60000UL) {
        stop(i, "Completed");
      }
    }
  }

  // Check schedule-based starts
  for (uint8_t i = 0; i < zoneN; i++) {
    Zone& z = zones[i];
    if (z.running) continue;
    if (!z.enabled) continue;
    if (z.ranToday) continue;
    if (!z.days[day7(n.dayOfTheWeek())]) continue;
    if (n.hour() != z.hour || n.minute() != z.minute) continue;
    z.ranToday = true;
    start(i, z.duration, "SCHEDULED");
  }
}

// --- persistence ---
void IrrigationService::save() {
  Preferences p;
  p.begin("irrigation", false);
  p.putUChar("count", zoneN);
  for (uint8_t i = 0; i < zoneN; i++) {
    String keyBase = "z" + String(i) + "_";
    const Zone& z = zones[i];
    p.putUChar((keyBase + "id").c_str(), z.id);
    p.putUChar((keyBase + "pin").c_str(), z.pin);
    p.putBool((keyBase + "en").c_str(), z.enabled);
    p.putUChar((keyBase + "h").c_str(), z.hour);
    p.putUChar((keyBase + "m").c_str(), z.minute);
    p.putUShort((keyBase + "dur").c_str(), z.duration);
    for (int d = 1; d <= 7; d++)
      p.putBool((keyBase + "d" + String(d)).c_str(), z.days[d]);
  }
  p.end();
}

void IrrigationService::load() {
  Preferences p;
  p.begin("irrigation", true);
  zoneN = p.getUChar("count", 0);
  if (zoneN > ZONE_COUNT) zoneN = ZONE_COUNT;
  for (uint8_t i = 0; i < zoneN; i++) {
    String keyBase = "z" + String(i) + "_";
    Zone& z = zones[i];
    z.id       = p.getUChar((keyBase + "id").c_str(), i + 1);
    z.pin      = p.getUChar((keyBase + "pin").c_str(), DEFAULT_PINS[i]);
    z.enabled  = p.getBool((keyBase + "en").c_str(), false);
    z.hour     = p.getUChar((keyBase + "h").c_str(), 8);
    z.minute   = p.getUChar((keyBase + "m").c_str(), 0);
    z.duration = p.getUShort((keyBase + "dur").c_str(), 15);
    for (int d = 1; d <= 7; d++)
      z.days[d] = p.getBool((keyBase + "d" + String(d)).c_str(), true);
    z.running = false;
    z.ranToday = false;
    z.started = 0;
  }
  p.end();
}

// --- begin/update ---
void IrrigationService::begin() {
  initRelays();
  pinMode(soilPowerPin, OUTPUT);
  pinMode(soilAdcPin, INPUT);
  digitalWrite(soilPowerPin, LOW);
  analogReadResolution(12);
  load();
  allRelaysOff();
}

void IrrigationService::update(uint32_t now) {
  readSoil(now);
  checkSchedules(now);
}

void IrrigationService::appendStatus(JsonObject object) const {
  JsonObject irr = object.createNestedObject("irrigation");
  irr["zoneCount"] = zoneN;
  irr["pumpOn"] = pump;
  irr["pumpManualOverride"] = pumpManualOverride;
  irr["pumpGpio"] = (RELAY_PINS[PUMP_RELAY_CHANNEL] >= 0) ? (int)RELAY_PINS[PUMP_RELAY_CHANNEL] : -1;
  irr["soilRaw"] = soilRaw;
  irr["soilPercent"] = soilPercent();

  JsonArray za = irr.createNestedArray("zones");
  for (uint8_t i = 0; i < zoneN; i++) {
    JsonObject zo = za.createNestedObject();
    const Zone& z = zones[i];
    zo["id"]         = z.id;
    zo["pin"]        = z.pin;
    zo["enabled"]    = z.enabled;
    zo["running"]    = z.running;
    zo["hour"]       = z.hour;
    zo["minute"]     = z.minute;
    zo["duration"]   = z.duration;
    zo["ranToday"]   = z.ranToday;
    zo["nextRun"]    = nextRun(i);

    uint32_t rem = 0;
    if (z.running) {
      uint32_t elapsed = (millis() >= z.started) ? (millis() - z.started) : 0;
      uint32_t total = (uint32_t)z.duration * 60000UL;
      rem = (elapsed < total) ? ((total - elapsed) / 1000) : 0;
    }
    zo["remainingSec"] = rem;
  }
}
