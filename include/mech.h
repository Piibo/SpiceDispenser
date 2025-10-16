#pragma once
#include <Arduino.h>
#include "config.h"

/*
  -----------------------------------------------------------------------------
  SpiceDispenser – Mechanical Control Interface
  -----------------------------------------------------------------------------
  Provides:
  - Initialization and configuration of motion system
  - Position management and dispensing control
  - Servo coupling/decoupling helpers
  -----------------------------------------------------------------------------
  Notes:
  - Pin definitions are provided via pins.h
  - Constants and speed profiles via config.h
  - All functions are safe to call from main control code
  -----------------------------------------------------------------------------
*/

// -----------------------------------------------------------------------------
// Core Motion Control
// -----------------------------------------------------------------------------
void mechInit();
void mechSetPosCount(int count);
int  mechGetPosCount();
int  mechGetCurrentIndex();

// -----------------------------------------------------------------------------
// High-Level Operations
// -----------------------------------------------------------------------------
void mechGotoIndex(int target_idx);
void mechDispenseRotations(float rotations);

// -----------------------------------------------------------------------------
// Servo Utilities
// Return the updated pulse width after movement.
// -----------------------------------------------------------------------------
uint32_t mechServoToBack(uint32_t cur);
uint32_t mechServoToFront(uint32_t cur);
uint32_t mechDecoupleBack(uint32_t cur);
