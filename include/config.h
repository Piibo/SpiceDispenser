#pragma once
#include <Arduino.h>

/*
  -----------------------------------------------------------------------------
  SpiceDispenser – Global Configuration
  Board: ESP32-C6 DevKit (Arduino)
  -----------------------------------------------------------------------------

  Contains:
  - Network credentials and AI service endpoints
  - Mechanical geometry and behavioral parameters
  - Stepper speed profiles
  - Servo timing configuration
  - Calibration table for dispensing weights
*/

/* ===========================
 *  WLAN & AI Service
 * =========================== */
inline constexpr const char* WIFI_SSID = "<YOUR_WIFI_SSID>";
inline constexpr const char* WIFI_PW   = "<YOUR_WIFI_PASSWORD>";
inline constexpr const char* AI_URL    = "http://<AI_SERVER_IP>:8000/spiceplan";
inline constexpr const char* AI_HEALTH = "http://<AI_SERVER_IP>:8000/health";

/* ===========================
 *  Mechanics / Geometry
 * =========================== */
inline constexpr long TRAVEL_STEPS_PER_REV = 438;
inline constexpr long HOME_OFFSET_STEPS    = 0;

inline constexpr int  DISPENSE_STEPS_PER_REV = 200;
inline constexpr int  POS_COUNT_DEFAULT      = 5;
inline constexpr int  MAX_POS                = 32;
inline constexpr int  SPICE_NAME_MAX         = 24;

/* ===========================
 *  Mechanical Behavior Flags
 * =========================== */
inline constexpr bool MECH_SHORTEST_PATH = false;
inline constexpr bool MECH_ALWAYS_CW     = false;
inline constexpr bool MECH_DIR_INVERT    = false;

/* ===========================
 *  Motion Profiles
 * =========================== */
struct speed_profile_t { int us_min; int us_max; int accel_per_step_us; };

inline constexpr speed_profile_t PROFILE_MOVE { 2500, 4000, 25 };
inline constexpr speed_profile_t PROFILE_DISP { 1200, 2600, 20 };

/* ===========================
 *  Servo Parameters
 * =========================== */
inline constexpr int      SERVO_FREQ_HZ         = 50;
inline constexpr uint32_t SERVO_BACK_US         = 910;
inline constexpr uint32_t SERVO_FRONT_US        = 1090;
inline constexpr int      SERVO_STEP_US         = 8;
inline constexpr int      SERVO_STEP_DELAY_MS   = 12;
inline constexpr int      SERVO_SETTLE_MS       = 200;
inline constexpr int      PAUSE_BEFORE_DECOUPLE = 250;
inline constexpr int      PAUSE_AFTER_COUPLE    = 150;
inline constexpr int      PAUSE_BEFORE_MOVE     = 120;
inline constexpr int      PAUSE_AFTER_DISP      = 80;

/* ===========================
 *  Calibration: grams per full screw rotation
 * =========================== */
inline float GRAMS_PER_ROTATION[MAX_POS] = {
  2.0f, 1.5f, 1.2f, 1.8f,   // 1..4
  1.5f, 1.5f, 1.5f, 1.5f,   // 5..8
  1.5f, 1.5f, 1.5f, 1.5f,   // 9..12
  1.5f, 1.5f, 1.5f, 1.5f,   // 13..16
  1.5f, 1.5f, 1.5f, 1.5f,   // 17..20
  1.5f, 1.5f, 1.5f, 1.5f,   // 21..24
  1.5f, 1.5f, 1.5f, 1.5f,   // 25..28
  1.5f, 1.5f, 1.5f, 1.5f    // 29..32
};

/* ===========================
 *  Compile-time Checks
 * =========================== */
static_assert(MAX_POS > 0, "MAX_POS must be positive.");
static_assert(sizeof(GRAMS_PER_ROTATION) / sizeof(GRAMS_PER_ROTATION[0]) == MAX_POS,
              "GRAMS_PER_ROTATION length must equal MAX_POS.");
