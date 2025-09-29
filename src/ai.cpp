#include "ai.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>

static const char* httpErrorStr(int code) {
  switch (code) {
    case -1:  return "connection_refused";
    case -2:  return "send_header_failed";
    case -3:  return "send_payload_failed";
    case -4:  return "not_connected";
    case -5:  return "connection_lost";
    case -6:  return "no_stream";
    case -7:  return "no_http_server";
    case -8:  return "too_less_ram";
    case -9:  return "encoding";
    case -10: return "stream_write";
    case -11: return "read_timeout";
    default:  return "unknown";
  }
}

bool wifi_connect(){
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);
  Serial.printf("[WIFI] SSID=%s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PW);
  uint32_t t0 = millis();
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] OK  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WIFI] FAILED (SSID/PW? 2.4GHz? WPA2/Mixed?)");
  return false;
}

bool ai_health(){
  if(!wifi_connect()) return false;
  HTTPClient http; http.begin(AI_HEALTH); http.setTimeout(5000);
  int code = http.GET(); String body = http.getString(); http.end();
  Serial.printf("[HTTP][health] code=%d (%s) body=%s\n", code, httpErrorStr(code), body.c_str());
  return code==200;
}

bool ai_post_plan(const String& dish, int servings, const char* intensity, JsonDocument& out){
  if(!wifi_connect()) return false;
  HTTPClient http; http.begin(AI_URL); http.addHeader("Content-Type","application/json"); http.setTimeout(15000);
  JsonDocument req; req["dish"]=dish; req["servings"]=servings; req["intensity"]=intensity;
  String body; serializeJson(req, body);
  int code=http.POST(body); String resp=http.getString(); http.end();
  Serial.printf("[HTTP][post] code=%d (%s) len=%d\n", code, httpErrorStr(code), resp.length());
  if (code != 200) { Serial.printf("[HTTP][post] body=%s\n", resp.c_str()); Serial.printf("{\"error\":\"http\",\"code\":%d}\n", code); return false; }
  auto err = deserializeJson(out, resp);
  if (err) { Serial.printf("[HTTP][json] parse_error=%s\n", err.c_str()); Serial.println("{\"error\":\"invalid_ai_json\"}"); return false; }
  return true;
}

bool ai_post_voice(const char* lang, int servings, const char* intensity, JsonDocument& out){
  if(!wifi_connect()) return false;
  String url = String(AI_URL); url.replace("/spiceplan", "/voiceplan");
  HTTPClient http; http.begin(url); http.addHeader("Content-Type","application/json"); http.setTimeout(15000);
  JsonDocument req; req["lang"]=lang; req["servings"]=servings; req["intensity"]=intensity;
  String body; serializeJson(req, body);
  Serial.printf("[HTTP][voice] POST %s\n", url.c_str());
  int code=http.POST(body); String resp=http.getString(); http.end();
  Serial.printf("[HTTP][voice] code=%d (%s) len=%d\n", code, httpErrorStr(code), resp.length());
  if (code != 200) { Serial.printf("{\"error\":\"http\",\"code\":%d}\n", code); return false; }
  auto err = deserializeJson(out, resp);
  if (err) { Serial.printf("[HTTP][voice] json_err=%s\n", err.c_str()); Serial.println("{\"error\":\"invalid_ai_json\"}"); return false; }
  return true;
}
