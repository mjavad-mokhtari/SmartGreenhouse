#include "modules/lighting/LightingService.h"
void LightingService::begin() { pinMode(channelPin, OUTPUT); digitalWrite(channelPin, HIGH); }
void LightingService::update(uint32_t) {}
void LightingService::set(bool on) { enabled = on; digitalWrite(channelPin, on ? LOW : HIGH); led.pulse(); }
void LightingService::appendStatus(JsonObject object) const { JsonObject state = object.createNestedObject("lighting"); state["enabled"] = enabled; state["channels"] = 1; }