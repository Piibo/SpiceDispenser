#pragma once
#include <Arduino.h>
#include "config.h"

// Pins kommen aus pins.h (STEP_PIN, DIR_PIN, EN_PIN, SERVO_PIN)
void mechInit();
void mechSetPosCount(int count);
int  mechGetPosCount();
int  mechGetCurrentIndex();

void mechGotoIndex(int target_idx);
void mechDispenseRotations(float rotations);

// Servo-Helfer (geben die neue Pulsbreite zurück)
uint32_t mechServoToBack(uint32_t cur);
uint32_t mechServoToFront(uint32_t cur);
uint32_t mechDecoupleBack(uint32_t cur);
