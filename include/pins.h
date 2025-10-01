#pragma once
#include <Arduino.h>

#define STEP_PIN   4 //gelb
#define DIR_PIN    5 //orange
#define EN_PIN     6 //grün

#define BTN_SERVO  8
#define SERVO_PIN  14 //weiß

// Rotary / Select-Button
#define BTN_SEL    22 //grün
#define ROT_CLK    2 //orange
#define ROT_DT     3 //gelb


//Display
#define OLED_CLK   19  //D0, SCL SPI clock 7l   //gelb
#define OLED_MOSI  23  //D1, SDA, SPI Data 8l   //orange
#define OLED_CS    18  //chip select 6r  //blau //lila
#define OLED_DC    21  //A0, data/command 4l    //braun
#define OLED_RST   20  // reset r5              //grün
