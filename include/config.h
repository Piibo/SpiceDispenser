#pragma once
#include <Arduino.h>

// --- WLAN & AI-Service ---
inline constexpr const char* WIFI_SSID = "YOUR_WIFI_SSID";
inline constexpr const char* WIFI_PW   = "YOUR_WIFI_PASSWORD";
inline constexpr const char* AI_URL    = "http://10.57.144.53:8000/spiceplan";
inline constexpr const char* AI_HEALTH = "http://10.57.144.53:8000/health";

// --- Mechanik / Geometrie ---
// Beispiel: ~88 Steps/Slot bei 5 Slots -> 5 * 88 = 440
inline constexpr long TRAVEL_STEPS_PER_REV = 440;
inline constexpr long HOME_OFFSET_STEPS    = 0;     // Feinoffset nach Homing (± wenige Steps)

inline constexpr int  DISPENSE_STEPS_PER_REV = 200; // Dosierschnecke (Steps/Rotation)
inline constexpr int  POS_COUNT_DEFAULT      = 5;
inline constexpr int  MAX_POS                = 32;
inline constexpr int  SPICE_NAME_MAX         = 24;

// --- Mechanik-Schalter ---
inline constexpr bool MECH_SHORTEST_PATH = false; // true = kürzester Weg (bei dir lieber AUS)
inline constexpr bool MECH_ALWAYS_CW     = false; // true = immer vorwärts (Uhrzeigersinn)
inline constexpr bool MECH_DIR_INVERT    = false; // true = DIR-Pin-Polarität invertieren

// --- Rampenprofile ---
struct speed_profile_t { int us_min; int us_max; int accel_per_step_us; };
// Deine bisherigen Werte:
inline constexpr speed_profile_t PROFILE_MOVE {2500, 4000, 25};
inline constexpr speed_profile_t PROFILE_DISP {1200, 2600, 20};

// --- Servo ---
inline constexpr int      SERVO_FREQ_HZ        = 50;
inline constexpr uint32_t SERVO_BACK_US        = 910;   // entkoppelt
inline constexpr uint32_t SERVO_FRONT_US       = 1090;  // gekoppelt
inline constexpr int      SERVO_STEP_US        = 8;
inline constexpr int      SERVO_STEP_DELAY_MS  = 12;
inline constexpr int      SERVO_SETTLE_MS      = 200;
inline constexpr int      PAUSE_BEFORE_DECOUPLE= 250;
inline constexpr int      PAUSE_AFTER_COUPLE   = 150;
inline constexpr int      PAUSE_BEFORE_MOVE    = 120;
inline constexpr int      PAUSE_AFTER_DISP     = 80;

// --- Kalibrierung g pro Umdrehung (pro Position) ---
inline float GRAMS_PER_ROTATION[MAX_POS] = {
  // Deine ersten vier:
  2.0f, 1.5f, 1.2f, 1.8f,   // 1..4
  // Rest Default (bitte je Slot später messen & anpassen):
  1.5f, 1.5f, 1.5f, 1.5f,   // 5..8
  1.5f, 1.5f, 1.5f, 1.5f,   // 9..12
  1.5f, 1.5f, 1.5f, 1.5f,   // 13..16
  1.5f, 1.5f, 1.5f, 1.5f,   // 17..20
  1.5f, 1.5f, 1.5f, 1.5f,   // 21..24
  1.5f, 1.5f, 1.5f, 1.5f,   // 25..28
  1.5f, 1.5f, 1.5f, 1.5f    // 29..32
};
