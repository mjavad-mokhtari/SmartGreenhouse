#ifndef SYNC_H
#define SYNC_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

struct SyncEvent {
  uint32_t id;
  String type;
  String state;
  uint32_t ts;
};

static const size_t SYNC_RING_SIZE = 50;
inline SyncEvent _syncEvents[SYNC_RING_SIZE];
inline size_t _syncHead = 0;
inline size_t _syncCount = 0;
inline uint32_t _syncNextId = 1;

inline Preferences _syncPrefs;
inline String _srvUrl = "";
inline bool _srvEnabled = false;
inline bool _serverOnline = false;
inline int _syncLastHttpCode = 0;
inline String _syncLastResult = "idle";
inline uint32_t _syncLastAttempt = 0;
inline uint32_t _syncBackoffMs = 30000;
static const uint32_t SYNC_MAX_BACKOFF_MS = 900000; // 15 min
inline bool _syncForceNow = false;
inline String _deviceId = "esp32-irrigation";

// Remote command polling

inline void syncLoadConfig() {
  _syncPrefs.begin("sync", false);
  _srvUrl = _syncPrefs.getString("srv_url", "");
  _srvEnabled = _syncPrefs.getBool("srv_en", false);
  _syncPrefs.end();
}

inline String getServerUrl() { return _srvUrl; }

inline void setServerUrl(const String& u) {
  _srvUrl = u;
  _syncPrefs.begin("sync", false);
  _syncPrefs.putString("srv_url", _srvUrl);
  _syncPrefs.end();
}

inline bool getServerEnabled() { return _srvEnabled; }

inline void setServerEnabled(bool en) {
  _srvEnabled = en;
  _syncPrefs.begin("sync", false);
  _syncPrefs.putBool("srv_en", _srvEnabled);
  _syncPrefs.end();
  if (!_srvEnabled) _serverOnline = false;
}

// --- API Key (MANDATORY for remote access) ---
inline String getApiKey() {
  _syncPrefs.begin("sync", false);
  String key = _syncPrefs.getString("api_key", "");
  _syncPrefs.end();
  return key;
}

inline void setApiKey(const String& key) {
  _syncPrefs.begin("sync", false);
  _syncPrefs.putString("api_key", key);
  _syncPrefs.end();
}

// Returns true only if a key IS set AND the provided key matches.
// Returns false if no key is set (unconfigured) OR key doesn't match.
inline bool checkApiKey(const String& provided) {
  String stored = getApiKey();
  if (stored.length() == 0) return false;   // MUST be configured
  return provided == stored;
}

inline String apiKeySummary() {
  String key = getApiKey();
  if (key.length() == 0) return "not-set";
  return key.substring(0,4) + "****";
}


inline bool serverOnlineFlag() { return _serverOnline; }
inline size_t pendingCount() { return _syncCount; }

inline String getDeviceId() { return _deviceId; }

inline void setDeviceId(const String& id) {
  if (id.length() > 0) _deviceId = id;
  _syncPrefs.begin("sync", false);
  _syncPrefs.putString("dev_id", _deviceId);
  _syncPrefs.end();
}

inline void syncLoadDeviceId() {
  _syncPrefs.begin("sync", false);
  _deviceId = _syncPrefs.getString("dev_id", "esp32-irrigation");
  _syncPrefs.end();
}

inline void queueEvent(const String& type, const String& state) {
  size_t idx = (_syncHead + _syncCount) % SYNC_RING_SIZE;
  if (_syncCount == SYNC_RING_SIZE) {
    _syncHead = (_syncHead + 1) % SYNC_RING_SIZE;
  } else {
    _syncCount++;
  }
  _syncEvents[idx].id = _syncNextId++;
  _syncEvents[idx].type = type;
  _syncEvents[idx].state = state;
  _syncEvents[idx].ts = millis();
}

inline void queueEvent(const char* type, const char* state) {
  queueEvent(String(type), String(state));
}

inline void syncTriggerNow() {
  _syncForceNow = true;
}

inline String syncStatusJson() {
  String out = "{";
  out += "\"server\":{";
  out += "\"url\":\"" + _srvUrl + "\",";
  out += "\"enabled\":" + String(_srvEnabled ? "true" : "false") + ",";
  out += "\"online\":" + String(_serverOnline ? "true" : "false") + ",";
  out += "\"pending\":" + String(_syncCount) + ",";
  out += "\"httpCode\":" + String(_syncLastHttpCode) + ",";
  out += "\"result\":\"" + _syncLastResult + "\",";
  out += "\"backoffMs\":" + String(_syncBackoffMs);
  out += "}}";
  return out;
}

inline String eventsJson() {
  String j = "[";
  size_t total = _syncCount;
  for (size_t i = 0; i < total; i++) {
    size_t idx = (_syncHead + i) % SYNC_RING_SIZE;
    if (i > 0) j += ",";
    j += "{\"id\":" + String(_syncEvents[idx].id);
    j += ",\"type\":\"" + String(_syncEvents[idx].type) + "\"";
    j += ",\"state\":\"" + String(_syncEvents[idx].state) + "\"";
    j += ",\"ts\":" + String(_syncEvents[idx].ts) + "}";
  }
  j += "]";
  return j;
}

inline bool pollServerCommands(String& outJson, uint32_t timeoutMs = 2500) {
  if (!_srvEnabled || WiFi.status() != WL_CONNECTED || _srvUrl.length() == 0) {
    return false;
  }

  HTTPClient http;
  String commandUrl = _srvUrl;
  if (!commandUrl.endsWith("/")) commandUrl += "/";
  commandUrl += "api/commands?device=" + _deviceId;

  http.begin(commandUrl);
  String key = getApiKey();
  if (key.length() > 0) {
    http.addHeader("X-API-Key", key);
  }
  http.setTimeout(timeoutMs);
  int code = http.GET();

  if (code < 200 || code >= 300) {
    http.end();
    return false;
  }

  outJson = http.getString();
  http.end();
  return true;
}

inline bool ackServerCommands(const String& idsJsonArray) {
  if (!_srvEnabled || WiFi.status() != WL_CONNECTED || _srvUrl.length() == 0) {
    return false;
  }

  HTTPClient http;
  String ackUrl = _srvUrl;
  if (!ackUrl.endsWith("/")) ackUrl += "/";
  ackUrl += "api/commands/ack";

  http.begin(ackUrl);
  String key = getApiKey();
  if (key.length() > 0) {
    http.addHeader("X-API-Key", key);
  }
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"deviceId\":\"" + _deviceId + "\",\"ids\":" + idsJsonArray + "}";
  int code = http.POST(payload);
  http.end();
  return (code >= 200 && code < 300);
}

inline void syncLoop(const String& statusJson = String()) {
  if (!_srvEnabled || WiFi.status() != WL_CONNECTED || _srvUrl.length() == 0) {
    _serverOnline = false;
    return;
  }

  // First, sync local ring buffer events.
  uint32_t now = millis();
  if (_syncForceNow || (now - _syncLastAttempt >= _syncBackoffMs)) {
    _syncForceNow = false;
    _syncLastAttempt = now;

    HTTPClient http;
    String fullUrl = _srvUrl;
    if (!fullUrl.endsWith("/")) fullUrl += "/";
    fullUrl += "api/events";

    http.begin(fullUrl);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    String key = getApiKey();
    if (key.length() > 0) http.addHeader("X-API-Key", key);

    String payload = "{";
    payload += "\"deviceId\":\"" + _deviceId + "\",";
    payload += "\"serverTime\":" + String(millis()) + ",";
    payload += "\"uptime\":" + String(millis()) + ",";
    payload += "\"pending\":" + String(_syncCount) + ",";
    if (statusJson.length() > 0) {
      payload += "\"status\":";
      payload += statusJson;
      payload += ",";
    }
    payload += "\"events\":[";
    for (size_t i = 0; i < _syncCount; i++) {
      size_t idx = (_syncHead + i) % SYNC_RING_SIZE;
      if (i > 0) payload += ",";
      payload += "{\"id\":" + String(_syncEvents[idx].id) + ",";
      payload += "\"type\":\"" + String(_syncEvents[idx].type) + "\",";
      payload += "\"state\":\"" + String(_syncEvents[idx].state) + "\",";
      payload += "\"ts\":" + String(_syncEvents[idx].ts) + "}";
    }
    payload += "]}";

    int code = http.POST(payload);
    _syncLastHttpCode = code;
    bool accepted = false;
    if (code >= 200 && code < 300) {
      DynamicJsonDocument reply(512);
      DeserializationError parseErr = deserializeJson(reply, http.getString());
      accepted = !parseErr && reply["ok"].as<bool>();
    }
    if (accepted) {
      _serverOnline = true;
      _syncLastResult = "accepted";
      _syncHead = 0;
      _syncCount = 0;
      _syncBackoffMs = 30000;
    } else {
      _serverOnline = false;
      _syncLastResult = (code >= 200 && code < 300) ? "invalid-response" : "http-error";
      if (_syncBackoffMs < SYNC_MAX_BACKOFF_MS) {
        _syncBackoffMs *= 2;
        if (_syncBackoffMs > SYNC_MAX_BACKOFF_MS) _syncBackoffMs = SYNC_MAX_BACKOFF_MS;
      }
    }
    http.end();
  }
}

#endif // SYNC_H
