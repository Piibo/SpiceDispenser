#pragma once
#include <Arduino.h>
#include "config.h"

// Initialisierung von Stepper & Servo
void mechInit();

// Positionsverwaltung
void mechSetPosCount(int count);
int  mechGetPosCount();
int  mechGetCurrentIndex();
void mechGotoIndex(int idx);

// Dosieren
void mechDispenseRotations(float rotations);

// Servo-Hilfen (Rückgabewert ist die neue Servoposition in µs)
uint32_t mechServoToBack(uint32_t cur);
uint32_t mechServoToFront(uint32_t cur);
uint32_t mechDecoupleBack(uint32_t cur); // mit Vor-Pause
