#include "mech.h"
#include "pins.h"
#include <ESP32Servo.h>
#include <math.h>

/*
  -----------------------------------------------------------------------------
  SpiceDispenser – Mechanical Control (implementation)
  -----------------------------------------------------------------------------
  - Stepper motion with simple trapezoidal timing (µs-based)
  - Absolute slot positioning (no quarter snapping for travel)
  - Dispensing with 1/4-step quantization + residual error compensation
  - Servo coupling/decoupling with smooth microsecond ramps
  -----------------------------------------------------------------------------
*/

// =======================
// Stepper timing
// =======================
static constexpr int STEP_PULSE_HIGH_US = 4;

// =======================
// State
// =======================
static Servo g_servo;

static long long g_abs_steps   = 0;
static int       g_current_idx = 0;                 // 0 .. g_pos_count-1
static int       g_pos_count   = POS_COUNT_DEFAULT; // current number of slots

// Quarter raster for dispensing only (NOT for travel)
static const long QUARTER_STEPS = (DISPENSE_STEPS_PER_REV / 4);

// Residual error per slot (in steps) to keep average exact over time
static double g_dispResidualSteps[MAX_POS] = {0.0};

// =======================
// GPIO helpers
// =======================
static inline void step_pulse_once() {
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(STEP_PULSE_HIGH_US);
  digitalWrite(STEP_PIN, LOW);
}

static inline void set_dir(int lvl) {
  digitalWrite(DIR_PIN, lvl ? HIGH : LOW);
  delayMicroseconds(5);
}

// =======================
// Ramped move (trapezoid)
// =======================
static void move_steps_ramped(int delta_steps, const speed_profile_t* p) {
  if (delta_steps == 0 || !p) return;

  int us_min = max(p->us_min, 200);
  int us_max = max(p->us_max, us_min);
  int d_us   = max(p->accel_per_step_us, 1);
  int cnt    = abs(delta_steps);

  // Direction incl. optional inversion
  int dirlvl = ((delta_steps > 0) ? 1 : 0) ^ (MECH_DIR_INVERT ? 1 : 0);
  set_dir(dirlvl);

  // Symmetric accel/plateau/decel
  int accel_steps = (us_max - us_min + d_us - 1) / d_us;
  int decel_steps = accel_steps;
  if (2 * accel_steps > cnt) {
    accel_steps = cnt / 2;
    decel_steps = cnt - accel_steps;
  }
  int plateau_steps = cnt - accel_steps - decel_steps;

  int us = us_max;
  int stepSign = (delta_steps > 0) ? +1 : -1;

  for (int i = 0; i < accel_steps;   i++) { step_pulse_once(); delayMicroseconds(us); g_abs_steps += stepSign; us = max(us - d_us, us_min); }
  for (int i = 0; i < plateau_steps; i++) { step_pulse_once(); delayMicroseconds(us); g_abs_steps += stepSign; }
  for (int i = 0; i < decel_steps;   i++) { step_pulse_once(); delayMicroseconds(us); g_abs_steps += stepSign; us = min(us + d_us, us_max); }
}

// =======================
// Helpers
// =======================
static inline long round_to_multiple(double value, long mult) {
  return lround(value / (double)mult) * mult;
}

// Slot geometry (drift-free, no quarter snapping)
// Travel goes to the exact slot position; only dispensing uses quarter raster.
static inline long slotAbsStepsRaw(int idx) {
  double s = (double)TRAVEL_STEPS_PER_REV * (double)idx / (double)g_pos_count;
  return (long)llround(s) + HOME_OFFSET_STEPS;
}

// =======================
// Public API
// =======================
void mechInit() {
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(EN_PIN,   OUTPUT);
  digitalWrite(EN_PIN, LOW);  // enable

  g_servo.setPeriodHertz(SERVO_FREQ_HZ);
  g_servo.attach(SERVO_PIN, 500, 2500);
  g_servo.writeMicroseconds((int)SERVO_BACK_US);

  g_abs_steps   = 0;
  g_current_idx = 0;
  g_pos_count   = POS_COUNT_DEFAULT;

  for (int i = 0; i < MAX_POS; ++i) g_dispResidualSteps[i] = 0.0;
}

void mechSetPosCount(int count) {
  g_pos_count = constrain(count, 1, MAX_POS);
}

int mechGetPosCount()     { return g_pos_count; }
int mechGetCurrentIndex() { return g_current_idx; }

void mechGotoIndex(int target_idx) {
  target_idx = constrain(target_idx, 0, max(0, g_pos_count - 1));

  // Absolute target for container travel (no quarter quantization)
  long curr_abs   = slotAbsStepsRaw(g_current_idx);
  long target_abs = slotAbsStepsRaw(target_idx);
  long delta      = target_abs - curr_abs;  // default: long route

  // Optional travel modes
  if (MECH_ALWAYS_CW) {
    if (delta < 0) delta += TRAVEL_STEPS_PER_REV; // always forward
  } else if (MECH_SHORTEST_PATH) {
    const long perRev  = TRAVEL_STEPS_PER_REV;
    const long halfRev = perRev / 2;
    if (delta >  halfRev) delta -= perRev;
    if (delta < -halfRev) delta += perRev;
  }

  Serial.printf("[MOVE] Pos%d -> Pos%d  steps=%ld\n",
                g_current_idx + 1, target_idx + 1, delta);

  move_steps_ramped((int)delta, &PROFILE_MOVE);
  g_current_idx = target_idx;
}

void mechDispenseRotations(float rotations) {
  if (rotations <= 0.f) return;

  // Desired steps from rotations
  double steps_f = (double)rotations * (double)DISPENSE_STEPS_PER_REV;

  // Add slot-specific residual for long-term accuracy
  int idx = g_current_idx;
  if (idx < 0 || idx >= MAX_POS) idx = 0;
  steps_f += g_dispResidualSteps[idx];

  // Hard quarter quantization for dispensing only
  long steps_q = round_to_multiple(steps_f, QUARTER_STEPS);

  // Update residual
  g_dispResidualSteps[idx] = steps_f - (double)steps_q;

  if (steps_q == 0) {
    Serial.printf("[DISP] @Pos%d: req=%.3f rot -> quantized to 0 (quarter=%ld)\n",
                  g_current_idx + 1, (double)rotations, QUARTER_STEPS);
    return;
  }

  double rot_q = (double)steps_q / (double)DISPENSE_STEPS_PER_REV;
  Serial.printf("[DISP] @Pos%d: req=%.3f rot -> quant=%.3f rot (%ld steps, quarter=%ld, resid=%.2f)\n",
                g_current_idx + 1, (double)rotations, rot_q, steps_q, QUARTER_STEPS,
                g_dispResidualSteps[idx]);

  move_steps_ramped((int)steps_q, &PROFILE_DISP);
}

// =======================
// Servo
// =======================
static inline void servo_write_us(uint32_t us) { g_servo.writeMicroseconds((int)us); }

static void servo_move_smooth(uint32_t from_us, uint32_t to_us) {
  if (from_us == to_us) { servo_write_us(from_us); return; }
  int step = (to_us > from_us) ? SERVO_STEP_US : -(int)SERVO_STEP_US;
  int pos  = (int)from_us;
  while ((step > 0 && pos < (int)to_us) || (step < 0 && pos > (int)to_us)) {
    pos += step;
    servo_write_us((uint32_t)pos);
    delay(SERVO_STEP_DELAY_MS);
  }
  servo_write_us(to_us);
}

uint32_t mechServoToBack(uint32_t cur) {
  if (cur != SERVO_BACK_US) { servo_move_smooth(cur, SERVO_BACK_US); cur = SERVO_BACK_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}

uint32_t mechServoToFront(uint32_t cur) {
  if (cur != SERVO_FRONT_US) { servo_move_smooth(cur, SERVO_FRONT_US); cur = SERVO_FRONT_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}

uint32_t mechDecoupleBack(uint32_t cur) {
  if (cur != SERVO_BACK_US) { delay(PAUSE_BEFORE_DECOUPLE); }
  return mechServoToBack(cur);
}
