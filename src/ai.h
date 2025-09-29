#pragma once
#include <ArduinoJson.h>
#include <WString.h>

bool wifi_connect();
bool ai_health();
bool ai_post_plan(const String& dish, int servings, const char* intensity, JsonDocument& out);
bool ai_post_voice(const char* lang, int servings, const char* intensity, JsonDocument& out);
