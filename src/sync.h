#ifndef SYNC_H
#define SYNC_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>

static const char SGH_LE_ROOT_CA[] PROGMEM = R"SGHCA(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)SGHCA";

inline bool beginServerRequest(HTTPClient& http, WiFiClientSecure& tlsClient, const String& url) {
  if (url.startsWith("https://")) {
    tlsClient.setCACert(SGH_LE_ROOT_CA);
    return http.begin(tlsClient, url);
  }
  return http.begin(url);
}

// Validate HTTPS before replacing the working server URL. This keeps a bad
// certificate, DNS result, or device clock from stranding remote management.
inline bool probeServerUrl(const String& baseUrl) {
  if (!baseUrl.startsWith("https://") || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  WiFiClientSecure tlsClient;
  String healthUrl = baseUrl;
  if (!healthUrl.endsWith("/")) healthUrl += "/";
  healthUrl += "api/health";
  if (!beginServerRequest(http, tlsClient, healthUrl)) return false;
  http.setTimeout(3500);
  const int code = http.GET();
  http.end();
  return code >= 200 && code < 300;
}

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

inline bool pollServerCommands(String& outJson, uint32_t timeoutMs = 1200) {
  if (!_srvEnabled || WiFi.status() != WL_CONNECTED || _srvUrl.length() == 0) {
    return false;
  }

  HTTPClient http;
  WiFiClientSecure tlsClient;
  String commandUrl = _srvUrl;
  if (!commandUrl.endsWith("/")) commandUrl += "/";
  commandUrl += "api/commands?device=" + _deviceId;

  if (!beginServerRequest(http, tlsClient, commandUrl)) return false;
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

inline bool ackServerCommand(const String& commandId, const String& status, const String& detail) {
  if (!_srvEnabled || WiFi.status() != WL_CONNECTED || _srvUrl.length() == 0) {
    return false;
  }

  HTTPClient http;
  WiFiClientSecure tlsClient;
  String ackUrl = _srvUrl;
  if (!ackUrl.endsWith("/")) ackUrl += "/";
  ackUrl += "api/commands/ack";

  if (!beginServerRequest(http, tlsClient, ackUrl)) return false;
  http.setTimeout(1200);
  String key = getApiKey();
  if (key.length() > 0) {
    http.addHeader("X-API-Key", key);
  }
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<384> doc;
  doc["deviceId"] = _deviceId;
  JsonArray results = doc.createNestedArray("results");
  JsonObject result = results.createNestedObject();
  result["id"] = commandId;
  result["status"] = status;
  result["detail"] = detail;
  String payload;
  serializeJson(doc, payload);
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
    WiFiClientSecure tlsClient;
    String fullUrl = _srvUrl;
    if (!fullUrl.endsWith("/")) fullUrl += "/";
    fullUrl += "api/events";

    if (!beginServerRequest(http, tlsClient, fullUrl)) {
      _serverOnline = false;
      _syncLastHttpCode = -1;
      _syncLastResult = "invalid-server-url";
      return;
    }
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
