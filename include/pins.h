#pragma once
#include <Arduino.h>

/*
  ---------------------------------------------------------------------------
  SpiceDispenser – Pin Map
  Board: ESP32-C6 DevKit (Arduino)
  ---------------------------------------------------------------------------
*/

/* ===========================
 *  Stepper Driver (A4988)
 * =========================== */
#define STEP_PIN   4   // STEP (yellow)
#define DIR_PIN    5   // DIR  (orange)
#define EN_PIN     6   // ENABLE (green) – Active LOW on A4988

/* ===========================
 *  Servo & Servo Button
 * =========================== */
#define BTN_SERVO  8   // Push button to toggle/actuate servo
#define SERVO_PIN  14  // Servo PWM (white)

/* ===========================
 *  Rotary Encoder + Select Button
 * =========================== */
#define BTN_SEL    22  // Encoder push button (green)
#define ROT_CLK    2   // Encoder A / CLK (yellow)
#define ROT_DT     3   // Encoder B / DT  (orange)

/* ===========================
 *  OLED Display (SPI-like)
 *  D0/D1 are the OLED’s labels (not I2C). We drive it via U8g2 “SPI” mode.
 * =========================== */
#define OLED_CLK   19  // D0 / SCL  (yellow)
#define OLED_MOSI  23  // D1 / SDA  (orange)
#define OLED_CS    18  // CS        (blue/purple)
#define OLED_DC    21  // A0 / DC   (brown)
#define OLED_RST   20  // RESET     (green)
