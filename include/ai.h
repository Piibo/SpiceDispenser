#pragma once
#include <ArduinoJson.h>
#include <WString.h>

/*
  -----------------------------------------------------------------------------
  AI / Network Interface (public API)
  -----------------------------------------------------------------------------
  Responsibilities:
    - Wi-Fi connection bootstrap
    - Health check for the AI backend
    - POST helpers for voice input and fixed-dish plans

  Notes:
    - Endpoints and credentials are configured in config.h
    - All functions are synchronous and return success via `bool`
    - On success, `out` is populated with the parsed JSON response
  -----------------------------------------------------------------------------
*/

/**
 * Connect to Wi-Fi using credentials from config.h.
 * @return true on successful connect; false otherwise.
 */
bool wifi_connect();

/**
 * Check the AI service health endpoint.
 * @return true if service is reachable and reports healthy; false otherwise.
 */
bool ai_health();

/**
 * Send a voice-captured request to the AI service.
 * Intended flow: show VOICE_INPUT → on_sending() → show VOICE_SEND.
 *
 * @param lang       Language code (e.g., "de", "en").
 * @param servings   Number of servings (>=1).
 * @param intensity  Flavor intensity string (e.g., "mild"|"medium"|"strong").
 * @param out        Parsed JSON response on success.
 * @param on_sending Optional callback invoked immediately before the HTTP POST.
 * @return true on success (and `out` filled); false on error.
 */
bool ai_post_voice(const char* lang,
                   int servings,
                   const char* intensity,
                   JsonDocument& out,
                   void (*on_sending)() = nullptr);

/**
 * Request a spice plan for a fixed dish name.
 *
 * @param dish       Dish name (UTF-8).
 * @param servings   Number of servings (>=1).
 * @param intensity  Flavor intensity string.
 * @param out        Parsed JSON response on success.
 * @param on_sending Optional callback invoked immediately before the HTTP POST.
 * @return true on success (and `out` filled); false on error.
 */
bool ai_post_plan(const char* dish,
                  int servings,
                  const char* intensity,
                  JsonDocument& out,
                  void (*on_sending)() = nullptr);
