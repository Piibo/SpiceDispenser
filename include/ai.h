#pragma once
#include <ArduinoJson.h>
#include <WString.h>

bool wifi_connect();
bool ai_health();
// Wird unmittelbar *vor* HTTP POST aufgerufen (nach VAD, direkt vor Senden).
bool ai_post_voice(const char* lang,
                   int servings,
                   const char* intensity,
                   JsonDocument& out,
                   void (*on_sending)() = nullptr);

bool ai_post_plan(const char* dish,
                  int servings,
                  const char* intensity,
                  JsonDocument& out,
                  void (*on_sending)() = nullptr);
