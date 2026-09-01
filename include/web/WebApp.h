#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "core/Services.h"
#ifdef MODULE_IRRIGATION
#include "modules/irrigation/IrrigationService.h"
#endif
#ifdef MODULE_LIGHTING
#include "modules/lighting/LightingService.h"
#endif

class WebApp {
public:
  WebApp(RtcManager& rtc, WifiManager& wifi
#ifdef MODULE_IRRIGATION
    , IrrigationService& irrigation
#endif
#ifdef MODULE_LIGHTING
    , LightingService& lighting
#endif
  );
  void begin();
  void update() {}
private:
  AsyncWebServer server{80};
  RtcManager& rtc;
  WifiManager& wifi;
#ifdef MODULE_IRRIGATION
  IrrigationService& irrigation;
#endif
#ifdef MODULE_LIGHTING
  LightingService& lighting;
#endif
  void sendStatus(AsyncWebServerRequest* request);
  static const char* dashboard();
};