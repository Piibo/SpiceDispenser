// spice_dispenser_arduino.ino
// Port von ESP-IDF nach Arduino (ESP32 / ESP32-C6 mit Arduino-ESP32 Core >= 3.x)

#include <Arduino.h>
#include <ESP32Servo.h>   // Bibliothek installieren!

// =================== Pins ===================
#define STEP_PIN    4
#define DIR_PIN     5
#define EN_PIN      6         // A4988: Enable aktiv LOW
#define BTN_STEP    7         // Start Zyklus
#define BTN_SERVO   8         // Servo manuell toggeln (optional)
#define SERVO_PIN   9         // Servo-Signal (PWM)

// =================== Konfig ===================
#define STEP_PULSE_HIGH_US          4

// === RAMPENPROFILE === (kleiner = schneller)
typedef struct {
  int us_min;            // schnellste Interstep-Zeit (Plateau)
  int us_max;            // langsamste Interstep-Zeit (Start/Ende)
  int accel_per_step_us; // Änderung je Schritt (linear)
} speed_profile_t;

// Fahrt (Wagen bewegen)
#define MOVE_US_MIN            2500
#define MOVE_US_MAX            4000
#define MOVE_ACCEL_PER_STEP      25

// Ausgabe (Spindel drehen)
#define DISP_US_MIN            1200
#define DISP_US_MAX            2600
#define DISP_ACCEL_PER_STEP      20

static const speed_profile_t PROFILE_MOVE = { MOVE_US_MIN,  MOVE_US_MAX,  MOVE_ACCEL_PER_STEP };
static const speed_profile_t PROFILE_DISP = { DISP_US_MIN,  DISP_US_MAX,  DISP_ACCEL_PER_STEP };

#define SERVO_FREQ_HZ       50
#define SERVO_BACK_US       910      // hinten  = Auslass-Kupplung
#define SERVO_FRONT_US      1090     // vorne  = Fahr-Kupplung
#define SERVO_STEP_US       8
#define SERVO_STEP_DELAY_MS 12
#define SERVO_SETTLE_MS     200

#define PAUSE_BEFORE_DECOUPLE 250    // ms: VOR jedem Auskoppeln (Fahrt -> Ausgabe)
#define PAUSE_AFTER_COUPLE    150
#define PAUSE_BEFORE_MOVE     120
#define PAUSE_AFTER_DISP      80

// === Mechanik ===
#define TRAVEL_STEPS_PER_STOP   88     // Schritte zwischen zwei Wagen-Positionen
// *** ANPASSEN an Microstepping/Getriebe: Vollschritt=200, 1/8=1600, 1/16=3200, ...
#define DISPENSE_STEPS_PER_REV  200

// === Wagenanzahl (konfigurierbar) ===
#define POS_COUNT_DEFAULT       4
#define MAX_POS                 32

// === Gewürz-Strings ===
#define SPICE_NAME_MAX          24

// === Mengenbegrenzung pro Position ===
#define MAX_DOSES_PER_POS       20

// === Routing-Strategie ===
#define ROUTE_KEEP_INPUT_ORDER  1      // 1 = Reihenfolge der ersten Nennung, 0 = nach Pos

// === Dosierfaktor global (Dose -> Umdrehungen) ===
#define ROTATIONS_PER_DOSE      1.0f   // z.B. 0.5 = halbe Umdr./Dose, 2.0 = zwei Umdr./Dose

// =================== Zustände ===================
static long long abs_steps = 0;   // nur fürs Logging
static int current_index = 0;     // 0..pos_count-1
static int pos_count = POS_COUNT_DEFAULT;

// Regal-Mapping (pro Wagen ein Name, lowercased gespeichert)
static char spices_map[MAX_POS][SPICE_NAME_MAX];

// Dosis-Faktor pro Position (Kalibrierung)
static float dose_factor_per_pos[MAX_POS];

// =================== Servo ===================
static Servo servo;            // aus ESP32Servo
static uint32_t servo_pos_us = SERVO_BACK_US;

// =================== Debounce ===================
typedef struct { uint8_t last_level; uint8_t stable_level; uint8_t stable_count; } deb_state_t;
static deb_state_t db_cycle = {1,1,0};
static deb_state_t db_servo = {1,1,0};

static int debounce_read(uint8_t pin, deb_state_t *st){
  int level = digitalRead(pin);
  if(level == st->last_level){ if(st->stable_count<5) st->stable_count++; }
  else { st->stable_count=0; st->last_level=level; }
  if(st->stable_count>=5) st->stable_level=level;
  return st->stable_level;
}
static bool edge_falling(uint8_t pin, deb_state_t *st){
  static uint8_t prev[64]={0};
  int val = debounce_read(pin,st);
  bool falling = (prev[pin]==HIGH && val==LOW);
  prev[pin]=val;
  return falling;
}

// =================== Helper ===================
static inline void step_pulse_once(){
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(STEP_PULSE_HIGH_US);
  digitalWrite(STEP_PIN, LOW);
}
static inline void set_dir(int lvl){
  digitalWrite(DIR_PIN, lvl ? HIGH : LOW);
  delayMicroseconds(5);
}

// Rampenbewegung in µs
static void move_steps_ramped(int delta_steps, const speed_profile_t *p){
  if(delta_steps == 0 || !p) return;

  int us_min = (p->us_min < 200) ? 200 : p->us_min;
  int us_max = (p->us_max < us_min) ? us_min : p->us_max;
  int d_us   = (p->accel_per_step_us < 1) ? 1 : p->accel_per_step_us;

  int dir  = (delta_steps>0) ? 1 : 0;
  int cnt  = (delta_steps>0) ?  delta_steps : -delta_steps;

  set_dir(dir);

  int accel_steps = (us_max - us_min + d_us - 1) / d_us; // aufrunden
  int decel_steps = accel_steps;

  if (2*accel_steps > cnt) {       // Triangular
    accel_steps = cnt / 2;
    decel_steps = cnt - accel_steps;
  }
  int plateau_steps = cnt - accel_steps - decel_steps;

  int us = us_max;

  for (int i=0; i<accel_steps; ++i){
    step_pulse_once(); delayMicroseconds(us);
    abs_steps += (dir? +1 : -1);
    us -= d_us; if (us < us_min) us = us_min;
  }
  for (int i=0; i<plateau_steps; ++i){
    step_pulse_once(); delayMicroseconds(us);
    abs_steps += (dir? +1 : -1);
  }
  for (int i=0; i<decel_steps; ++i){
    step_pulse_once(); delayMicroseconds(us);
    abs_steps += (dir? +1 : -1);
    us += d_us; if (us > us_max) us = us_max;
  }
}

// Servo smooth
static void servo_write_us(uint32_t us){
  servo.writeMicroseconds((int)us);
}
static void servo_move_smooth(uint32_t from_us,uint32_t to_us){
  if(from_us==to_us){ servo_write_us(from_us); return; }
  int step = (to_us>from_us)? SERVO_STEP_US : -(int)SERVO_STEP_US;
  int pos=(int)from_us;
  while((step>0 && pos<(int)to_us) || (step<0 && pos>(int)to_us)){
    pos+=step; servo_write_us((uint32_t)pos);
    delay(SERVO_STEP_DELAY_MS);
  }
  servo_write_us(to_us);
}
static uint32_t servo_to_back(uint32_t cur){
  if(cur!=SERVO_BACK_US){ servo_move_smooth(cur,SERVO_BACK_US); cur=SERVO_BACK_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}
static uint32_t servo_to_front(uint32_t cur){
  if(cur!=SERVO_FRONT_US){ servo_move_smooth(cur,SERVO_FRONT_US); cur=SERVO_FRONT_US; }
  delay(SERVO_SETTLE_MS);
  return cur;
}
static uint32_t decouple_to_back_with_pause(uint32_t cur){
  if(cur != SERVO_BACK_US){ delay(PAUSE_BEFORE_DECOUPLE); }
  return servo_to_back(cur);
}

// =================== Bewegungen (Positionslogik) ===================
static void goto_index(int target_idx){
  if(target_idx < 0) target_idx = 0;
  if(target_idx >= pos_count) target_idx = pos_count - 1;

  int delta_idx   = target_idx - current_index;
  int delta_steps = delta_idx * TRAVEL_STEPS_PER_STOP;

  Serial.printf("[MOVE] Pos%d -> Pos%d  steps=%d\n",
                current_index+1, target_idx+1, delta_steps);

  move_steps_ramped(delta_steps, &PROFILE_MOVE);
  current_index = target_idx;
}

// Einzelumdrehung (Test)
static void dispense_one_full_rev(void){
  Serial.printf("[DISP] @Pos%d: 1 rev (%d steps)\n", current_index+1, DISPENSE_STEPS_PER_REV);
  move_steps_ramped(DISPENSE_STEPS_PER_REV, &PROFILE_DISP);
}

// Beliebige Anzahl Umdrehungen zusammenhängend
static void dispense_rotations(float rotations){
  if (rotations <= 0.f) return;
  int steps = (int)(rotations * (float)DISPENSE_STEPS_PER_REV + 0.5f);
  Serial.printf("[DISP] @Pos%d: rotations=%.3f -> steps=%d\n", current_index+1, rotations, steps);
  move_steps_ramped(steps, &PROFILE_DISP);
}

// =================== Regal-Mapping & Matching ===================
static void spice_clear_all(void){
  for(int i=0;i<MAX_POS;i++) spices_map[i][0]='\0';
}
static void lowercase_inplace(char *s){
  for(size_t i=0; s && s[i]; ++i) s[i]=(char)tolower((unsigned char)s[i]);
}
static void spice_set(int idx, const char *name){
  if(idx<0 || idx>=MAX_POS || !name) return;
  strncpy(spices_map[idx], name, SPICE_NAME_MAX-1);
  spices_map[idx][SPICE_NAME_MAX-1]='\0';
  lowercase_inplace(spices_map[idx]); // normalisiert
}
static int find_pos_by_name(const char *name){
  if(!name) return -1;
  char tmp[SPICE_NAME_MAX];
  strncpy(tmp, name, SPICE_NAME_MAX-1);
  tmp[SPICE_NAME_MAX-1]='\0';
  lowercase_inplace(tmp);
  for(int i=0;i<pos_count;i++){
    if(spices_map[i][0]=='\0') continue;
    // strcasecmp ist auf ESP32 verfügbar:
    if(strcasecmp(spices_map[i], tmp)==0) return i;
  }
  return -1;
}

// =================== Mengenanforderungen ===================
typedef struct { const char *name; int doses; } spice_req_t;
typedef struct { int idx; int doses; } target_count_t;

/**
 * Ziele mit Mengen (doses) erzeugen:
 *  - summiert gleiche Positionen
 *  - cap auf MAX_DOSES_PER_POS
 *  - Reihenfolge: erste Nennung (oder sortiert nach Pos)
 */
static int build_targets_with_doses(const spice_req_t *reqs, int req_len,
                                    target_count_t *out, int out_max)
{
  if(!reqs || !out || out_max<=0) return 0;

  int counts[MAX_POS]={0};
  bool seen[MAX_POS]={0};
#if ROUTE_KEEP_INPUT_ORDER
  int order[MAX_POS]; int order_n=0;
#endif

  Serial.printf("[MATCH] %d Eintraege\n", req_len);

  for(int i=0;i<req_len;i++){
    int want = reqs[i].doses;
    if(want <= 0) continue;
    int pos = find_pos_by_name(reqs[i].name);
    if(pos<0){
      Serial.printf("  - fehlt: '%s' (%dx)\n", reqs[i].name, want);
      continue;
    }
    if(!seen[pos]){
      seen[pos]=true;
#if ROUTE_KEEP_INPUT_ORDER
      order[order_n++] = pos;
#endif
    }
    long sum = (long)counts[pos] + (long)want;
    if(sum > MAX_DOSES_PER_POS) sum = MAX_DOSES_PER_POS;
    counts[pos] = (int)sum;
    Serial.printf("  + match: '%s' -> Pos%d (+%d) = %dx\n",
                  reqs[i].name, pos+1, want, counts[pos]);
  }

  int n=0;
#if ROUTE_KEEP_INPUT_ORDER
  for(int k=0;k<order_n && n<out_max;k++){
    int p = order[k];
    if(counts[p]>0) out[n++] = (target_count_t){ p, counts[p] };
  }
#else
  for(int p=0;p<pos_count && n<out_max;p++){
    if(counts[p]>0) out[n++] = (target_count_t){ p, counts[p] };
  }
#endif
  Serial.printf("[MATCH] targets=%d\n", n);
  return n;
}

// =================== Zyklen ===================
static void run_cycle_targets_with_doses(uint32_t *p_servo_pos,
                                         const target_count_t *tg, int n){
  if(!p_servo_pos || !tg || n<=0) return;

  if(pos_count < 1) pos_count = 1;
  if(pos_count > MAX_POS) pos_count = MAX_POS;

  // Auf "Fahren" kuppeln
  *p_servo_pos = servo_to_front(*p_servo_pos);
  delay(PAUSE_AFTER_COUPLE);

  for(int i=0;i<n;i++){
    int idx   = tg[i].idx;
    int doses = tg[i].doses;
    if(idx<0 || idx>=pos_count || doses<=0) continue;

    goto_index(idx);
    delay(PAUSE_BEFORE_MOVE);

    *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);

    float rot = (float)doses * ROTATIONS_PER_DOSE * dose_factor_per_pos[idx];
    Serial.printf("[DOSE] Pos%d: doses=%d, faktor=%.2f -> rot=%.3f\n",
                  idx+1, doses, dose_factor_per_pos[idx], rot);
    dispense_rotations(rot);
    delay(PAUSE_AFTER_DISP);

    *p_servo_pos = servo_to_front(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);
  }

  // zurück zu Start + Parken
  goto_index(0);
  *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
  Serial.println("[DONE] Pos1 erreicht, Servo hinten (Park/Start).");
}

// Optional: alle Wagen 1× (Test)
static void run_cycle_all(uint32_t *p_servo_pos){
  if(!p_servo_pos) return;
  if(pos_count < 1) pos_count = 1;
  if(pos_count > MAX_POS) pos_count = MAX_POS;

  *p_servo_pos = servo_to_front(*p_servo_pos);
  delay(PAUSE_AFTER_COUPLE);

  for(int idx = 0; idx < pos_count; ++idx){
    goto_index(idx);
    delay(PAUSE_BEFORE_MOVE);

    *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);

    dispense_one_full_rev();
    delay(PAUSE_AFTER_DISP);

    *p_servo_pos = servo_to_front(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);
  }

  goto_index(0);
  *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
  Serial.println("[DONE] Zyklus all.");
}

// =================== Demo-Listen (mit Mengen) ===================
static const spice_req_t RECIPE_A_Q[] = {
  { "salz", 4 }, { "paprika", 2 }, { "rosmarin", 3 }, { "pfeffer", 1 }
};
static const int RECIPE_A_Q_LEN = sizeof(RECIPE_A_Q)/sizeof(RECIPE_A_Q[0]);

static const spice_req_t RECIPE_B_Q[] = {
  { "rosmarin", 5 }, { "salz", 4 }, { "salz", 1 }, { "paprika", 2 }
};
static const int RECIPE_B_Q_LEN = sizeof(RECIPE_B_Q)/sizeof(RECIPE_B_Q[0]);

// =================== Setup/Loop ===================
void setup(){
  Serial.begin(115200);
  delay(200);

  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(EN_PIN,   OUTPUT);
  digitalWrite(EN_PIN, LOW); // Enable aktiv

  pinMode(BTN_STEP,  INPUT_PULLUP);
  pinMode(BTN_SERVO, INPUT_PULLUP);

  // Servo
  servo.setPeriodHertz(SERVO_FREQ_HZ);
  servo.attach(SERVO_PIN, 500, 2500); // min/max µs
  servo_pos_us = SERVO_BACK_US;       // Park/Start
  servo.writeMicroseconds((int)servo_pos_us);

  // Startzustand
  pos_count = POS_COUNT_DEFAULT;
  current_index = 0;
  abs_steps = 0;

  // Regal-Mapping
  for(int i=0;i<MAX_POS;i++) dose_factor_per_pos[i]=1.0f;
  spice_clear_all();
  spice_set(0, "salz");
  spice_set(1, "pfeffer");
  spice_set(2, "rosmarin");
  spice_set(3, "paprika");

  Serial.printf("[BOOT] POS_COUNT=%d, TRAVEL=%d, DISP_REV=%d\n",
                pos_count, TRAVEL_STEPS_PER_STOP, DISPENSE_STEPS_PER_REV);
}

void loop(){
  static int which_list = 0;
  target_count_t targets[MAX_POS];

  if(edge_falling(BTN_STEP,&db_cycle)){
    const spice_req_t *reqs = (which_list==0)? RECIPE_A_Q : RECIPE_B_Q;
    int req_len             = (which_list==0)? RECIPE_A_Q_LEN : RECIPE_B_Q_LEN;
    Serial.printf("[BTN] Rezeptliste %s (mit Mengen)\n", (which_list==0)?"A":"B");

    int n_targets = build_targets_with_doses(reqs, req_len, targets, MAX_POS);
    if(n_targets>0) run_cycle_targets_with_doses(&servo_pos_us, targets, n_targets);
    else            Serial.println("[WARN] Keine passenden Gewuerze -> kein Lauf.");

    which_list ^= 1;
  }

  if(edge_falling(BTN_SERVO,&db_servo)){
    Serial.println("[BTN] Servo Toggle");
    if(servo_pos_us==SERVO_BACK_US) servo_pos_us = servo_to_front(servo_pos_us);
    else                            servo_pos_us = decouple_to_back_with_pause(servo_pos_us);
  }

  delay(10);
}
