#pragma once
#include <Arduino.h>

// --- WLAN & AI-Service ---
inline constexpr const char* WIFI_SSID = "YOUR_WIFI_SSID";
inline constexpr const char* WIFI_PW   = "YOUR_WIFI_PASSWORD";
inline constexpr const char* AI_URL    = "http://192.168.0.100:8000/spiceplan";
inline constexpr const char* AI_HEALTH = "http://192.168.0.100:8000/health";

// --- Pins / IO-Timings sind in pins.h bzw. mech.cpp ---

// --- Mechanik / Geometrie ---
inline constexpr int TRAVEL_STEPS_PER_STOP  = 88;
inline constexpr int DISPENSE_STEPS_PER_REV = 200;
inline constexpr int POS_COUNT_DEFAULT      = 4;
inline constexpr int MAX_POS                = 32;
inline constexpr int SPICE_NAME_MAX         = 24;

// --- Rampenprofile ---
struct speed_profile_t { int us_min; int us_max; int accel_per_step_us; };
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
  2.0, 1.5, 1.2, 1.8 // Pos 1..4 (anpassen/erweitern)
};
