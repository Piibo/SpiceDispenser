#pragma once
#include <Arduino.h>

#define STEP_PIN   4
#define DIR_PIN    5
#define EN_PIN     6

#define BTN_SERVO  8
#define SERVO_PIN  9

// Rotary / Select-Button
#define BTN_SEL    22
#define ROT_CLK    2
#define ROT_DT     3

//Display
#define OLED_CLK   19  //D0, SCL SPI clock 7l
#define OLED_MOSI  23  //D1, SDA, SPI Data 8l
#define OLED_CS    18  //chip select 6r
#define OLED_DC    21  //A0, data/command 4l
#define OLED_RST   14  // reset r5
