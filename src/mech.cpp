#include "mech.h"
#include "pins.h"
#include <ESP32Servo.h>

static constexpr int STEP_PULSE_HIGH_US = 4;

static Servo g_servo;
static long long g_abs_steps = 0;
static int g_current_index   = 0;
static int g_pos_count       = POS_COUNT_DEFAULT;

static inline void step_pulse_once(){
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(STEP_PULSE_HIGH_US);
  digitalWrite(STEP_PIN, LOW);
}
static inline void set_dir(int lvl){
  digitalWrite(DIR_PIN, lvl ? HIGH : LOW);
  delayMicroseconds(5);
}

void mechInit(){
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(EN_PIN,   OUTPUT);
  digitalWrite(EN_PIN, LOW);

  g_servo.setPeriodHertz(SERVO_FREQ_HZ);
  g_servo.attach(SERVO_PIN, 500, 2500);
  g_servo.writeMicroseconds((int)SERVO_BACK_US);

  g_abs_steps = 0;
  g_current_index = 0;
  g_pos_count = POS_COUNT_DEFAULT;
}

void mechSetPosCount(int count){
  g_pos_count = constrain(count, 1, MAX_POS);
}
int mechGetPosCount(){ return g_pos_count; }
int mechGetCurrentIndex(){ return g_current_index; }

static void move_steps_ramped(int delta_steps, const speed_profile_t* p){
  if(delta_steps == 0 || !p) return;
  int us_min = max(p->us_min, 200);
  int us_max = max(p->us_max, us_min);
  int d_us   = max(p->accel_per_step_us, 1);
  int dir    = (delta_steps>0) ? 1 : 0;
  int cnt    = abs(delta_steps);
  set_dir(dir);

  int accel_steps = (us_max - us_min + d_us - 1) / d_us;
  int decel_steps = accel_steps;
  if (2*accel_steps > cnt) { accel_steps = cnt/2; decel_steps = cnt - accel_steps; }
  int plateau_steps = cnt - accel_steps - decel_steps;
  int us = us_max;

  for (int i=0;i<accel_steps;i++){ step_pulse_once(); delayMicroseconds(us); g_abs_steps += (dir? +1 : -1); us = max(us - d_us, us_min); }
  for (int i=0;i<plateau_steps;i++){ step_pulse_once(); delayMicroseconds(us); g_abs_steps += (dir? +1 : -1); }
  for (int i=0;i<decel_steps;i++){ step_pulse_once(); delayMicroseconds(us); g_abs_steps += (dir? +1 : -1); us = min(us + d_us, us_max); }
}

void mechGotoIndex(int target_idx){
  target_idx = constrain(target_idx, 0, max(0,g_pos_count-1));
  int delta_steps = (target_idx - g_current_index) * TRAVEL_STEPS_PER_STOP;
  Serial.printf("[MOVE] Pos%d -> Pos%d  steps=%d\n", g_current_index+1, target_idx+1, delta_steps);
  move_steps_ramped(delta_steps, &PROFILE_MOVE);
  g_current_index = target_idx;
}

void mechDispenseRotations(float rotations){
  if (rotations <= 0.f) return;
  int steps = (int)(rotations * (float)DISPENSE_STEPS_PER_REV + 0.5f);
  Serial.printf("[DISP] @Pos%d: rotations=%.3f -> steps=%d\n", g_current_index+1, rotations, steps);
  move_steps_ramped(steps, &PROFILE_DISP);
}

static inline void servo_write_us(uint32_t us){ g_servo.writeMicroseconds((int)us); }
static void servo_move_smooth(uint32_t from_us,uint32_t to_us){
  if(from_us==to_us){ servo_write_us(from_us); return; }
  int step = (to_us>from_us)? SERVO_STEP_US : -(int)SERVO_STEP_US; int pos=(int)from_us;
  while((step>0 && pos<(int)to_us) || (step<0 && pos>(int)to_us)){
    pos+=step; servo_write_us((uint32_t)pos); delay(SERVO_STEP_DELAY_MS);
  }
  servo_write_us(to_us);
}
uint32_t mechServoToBack(uint32_t cur){
  if(cur!=SERVO_BACK_US){ servo_move_smooth(cur,SERVO_BACK_US); cur=SERVO_BACK_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}
uint32_t mechServoToFront(uint32_t cur){
  if(cur!=SERVO_FRONT_US){ servo_move_smooth(cur,SERVO_FRONT_US); cur=SERVO_FRONT_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}
uint32_t mechDecoupleBack(uint32_t cur){
  if(cur != SERVO_BACK_US){ delay(PAUSE_BEFORE_DECOUPLE); }
  return mechServoToBack(cur);
}
