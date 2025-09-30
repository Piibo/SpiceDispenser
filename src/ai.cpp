#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "ai.h"
#include "config.h"   // erwartet: WIFI_SSID, WIFI_PW, AI_URL, AI_HEALTH

// ---------- kleine Log-Helper ----------
static void log_snippet(const char* tag, const String& s, size_t maxlen = 512) {
  size_t n = s.length();
  size_t m = n < maxlen ? n : maxlen;
  Serial.printf("[%s] len=%u, snippet[0..%u]: ", tag, (unsigned)n, (unsigned)m);
  for (size_t i=0;i<m;i++) Serial.print((char)s[i]);
  if (n > m) Serial.print(" ...");
  Serial.println();
}

static void log_doc_basic(const JsonDocument& doc, const char* who) {
  const char* title = doc["title"].is<const char*>() ? doc["title"].as<const char*>() : "(null)";
  const char* dish  = doc["dish" ].is<const char*>() ? doc["dish" ].as<const char*>() : "(null)";
  Serial.printf("[JSON/%s] keys: title=%s, dish=%s\n", who, title, dish);

  JsonArrayConst arr = doc["spices"].as<JsonArrayConst>();
  if (arr.isNull()) {
    Serial.printf("[JSON/%s] spices=null or not array\n", who);
  } else {
    Serial.printf("[JSON/%s] spices.size()=%u\n", who, (unsigned)arr.size());
    uint16_t i=0;
    for (JsonObjectConst sp : arr) {
      const char* nm = sp["name"] | "(null)";
      double g = sp["grams"] | -1.0;
      Serial.printf("  [%u] name='%s' grams=%.3f\n", i++, nm, g);
    }
  }

  if (doc["text"].is<const char*>())
    Serial.printf("[JSON/%s] text='%s'\n", who, doc["text"].as<const char*>());
  if (doc["transcript"].is<const char*>())
    Serial.printf("[JSON/%s] transcript='%s'\n", who, doc["transcript"].as<const char*>());
}

// ---------- WLAN + Health (DEFINITIONEN) ----------
bool wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);

  Serial.printf("[WIFI] SSID=%s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PW);

  uint32_t t0 = millis();
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] OK  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  } else {
    Serial.println("[WIFI] FAILED (SSID/PW? 2.4GHz? WPA2/Mixed?)");
    return false;
  }
}

bool ai_health() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP][health] WiFi not connected -> reconnect");
    if (!wifi_connect()) return false;
  }

  HTTPClient http;
  http.begin(AI_HEALTH);
  http.setTimeout(5000);

  int code = http.GET();
  String body = http.getString();
  http.end();

  Serial.printf("[HTTP][health] code=%d body=%s\n", code, body.c_str());
  return (code == 200);
}

// interner Helper
static bool ensure_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[AI] WiFi not connected -> reconnect");
  return wifi_connect();
}

// ---------- AI Calls ----------
bool ai_post_voice(const char* lang,
                   int servings,
                   const char* intensity,
                   JsonDocument& out,
                   void (*on_sending)()) {
  if (!ensure_wifi()) { Serial.println("[AI][voice] wifi_not_connected"); return false; }

  String url = String(AI_URL);
  url.replace("/spiceplan", "/voiceplan");

  JsonDocument req;
  req["lang"]      = lang;
  req["servings"]  = servings;
  req["intensity"] = intensity;

  String body; serializeJson(req, body);
  Serial.printf("[AI][voice] POST %s\n", url.c_str());
  Serial.printf("[AI][voice] request: "); serializeJson(req, Serial); Serial.println();

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);

  if (on_sending) { Serial.println("[AI][voice] on_sending()"); on_sending(); }

  uint32_t t0 = millis();
  int code = http.POST(body);
  String resp = http.getString();
  uint32_t dt = millis() - t0;
  http.end();

  Serial.printf("[AI][voice] code=%d, dt=%lums\n", code, dt);
  log_snippet("AI/voice/resp", resp, 800);

  if (code != 200) {
    Serial.printf("[AI][voice] http_error code=%d\n", code);
    return false;
  }

  DeserializationError err = deserializeJson(out, resp);
  if (err) {
    Serial.printf("[AI][voice] json_err=%s\n", err.c_str());
    return false;
  }
  log_doc_basic(out, "voice");
  return true;
}

bool ai_post_plan(const char* dish,
                  int servings,
                  const char* intensity,
                  JsonDocument& out,
                  void (*on_sending)()) {
  if (!ensure_wifi()) { Serial.println("[AI][plan] wifi_not_connected"); return false; }

  String url = String(AI_URL);

  JsonDocument req;
  req["dish"]      = dish;
  req["servings"]  = servings;
  req["intensity"] = intensity;

  String body; serializeJson(req, body);
  Serial.printf("[AI][plan] POST %s\n", url.c_str());
  Serial.printf("[AI][plan] request: "); serializeJson(req, Serial); Serial.println();

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  if (on_sending) { Serial.println("[AI][plan] on_sending()"); on_sending(); }

  uint32_t t0 = millis();
  int code = http.POST(body);
  String resp = http.getString();
  uint32_t dt = millis() - t0;
  http.end();

  Serial.printf("[AI][plan] code=%d, dt=%lums\n", code, dt);
  log_snippet("AI/plan/resp", resp, 800);

  if (code != 200) {
    Serial.printf("[AI][plan] http_error code=%d\n", code);
    return false;
  }

  DeserializationError err = deserializeJson(out, resp);
  if (err) {
    Serial.printf("[AI][plan] json_err=%s\n", err.c_str());
    return false;
  }
  log_doc_basic(out, "plan");
  return true;
}
