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
  void update();

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

  // Response helpers
  void sendJson(AsyncWebServerRequest* request, const String& body, int code = 200);
  void sendJsonObj(AsyncWebServerRequest* request, JsonDocument& doc, int code = 200);

  // Endpoints
  void handleStatus(AsyncWebServerRequest* request);
  void handleHealth(AsyncWebServerRequest* request);
  void handleDashboard(AsyncWebServerRequest* request);

#ifdef MODULE_IRRIGATION
  void handleZoneOn(AsyncWebServerRequest* request);
  void handleZoneOff(AsyncWebServerRequest* request);
  void handleZoneAdd(AsyncWebServerRequest* request);
  void handleZoneSchedule(AsyncWebServerRequest* request);
  void handlePumpOn(AsyncWebServerRequest* request);
  void handlePumpOff(AsyncWebServerRequest* request);
  void handleAllOff(AsyncWebServerRequest* request);
#endif

#ifdef MODULE_LIGHTING
  void handleLightToggle(AsyncWebServerRequest* request);
  void handleLightAllOn(AsyncWebServerRequest* request);
  void handleLightAllOff(AsyncWebServerRequest* request);
#endif

  void handleConfigTime(AsyncWebServerRequest* request);
  void handleLogs(AsyncWebServerRequest* request);
  void handleConfigWifi(AsyncWebServerRequest* request);

  String buildStatusJson();
  String buildFullDashboard();
};
