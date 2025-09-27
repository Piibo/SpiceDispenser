// ======================= Spice Dispenser (ESP32-C6, Arduino) =======================
// Variante A: Gerät (mit UI) ruft AI als HTTP-Service (FastAPI) auf.
// Abhängigkeiten (PlatformIO lib_deps): ESP32Servo, ArduinoJson
// Board: esp32-c6-devkitm-1  |  Framework: arduino

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>
#include <cstring>
#include <ctype.h>

// ------------------- WLAN & AI-Service -------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PW   = "YOUR_WIFI_PASSWORD";
const char* AI_URL    = "http://192.168.178.35:8000/spiceplan";  // <--- PC/RPi-IP + Port

// ------------------- Pins (bei Bedarf anpassen) -------------------
#define STEP_PIN    4
#define DIR_PIN     5
#define EN_PIN      6
#define BTN_STEP    7    // Trigger: AI-Auftrag starten (Test)
#define BTN_SERVO   8    // Servo manuell toggeln (Service)
#define SERVO_PIN   9

// ------------------- Stepper / Rampen -------------------
#define STEP_PULSE_HIGH_US  4

typedef struct {
  int us_min; int us_max; int accel_per_step_us;
} speed_profile_t;

#define MOVE_US_MIN            2500
#define MOVE_US_MAX            4000
#define MOVE_ACCEL_PER_STEP      25

#define DISP_US_MIN            1200
#define DISP_US_MAX            2600
#define DISP_ACCEL_PER_STEP      20

static const speed_profile_t PROFILE_MOVE = { MOVE_US_MIN,  MOVE_US_MAX,  MOVE_ACCEL_PER_STEP };
static const speed_profile_t PROFILE_DISP = { DISP_US_MIN,  DISP_US_MAX,  DISP_ACCEL_PER_STEP };

// ------------------- Servo -------------------
#define SERVO_FREQ_HZ       50
#define SERVO_BACK_US       910    // "hinten" (entkoppelt)
#define SERVO_FRONT_US      1090   // "vorne"  (gekoppelt)
#define SERVO_STEP_US       8
#define SERVO_STEP_DELAY_MS 12
#define SERVO_SETTLE_MS     200

#define PAUSE_BEFORE_DECOUPLE 250
#define PAUSE_AFTER_COUPLE    150
#define PAUSE_BEFORE_MOVE     120
#define PAUSE_AFTER_DISP      80

// ------------------- Mechanik / Geometrie -------------------
#define TRAVEL_STEPS_PER_STOP   88
#define DISPENSE_STEPS_PER_REV  200

#define POS_COUNT_DEFAULT       4
#define MAX_POS                 32

#define SPICE_NAME_MAX          24
#define MAX_DOSES_PER_POS       20
#define ROTATIONS_PER_DOSE      1.0f

// Gramm pro 1.0 Umdrehung an Position i -> rotations = grams / GRAMS_PER_ROTATION[i]
static float GRAMS_PER_ROTATION[MAX_POS] = {
  2.0,  // Pos1  (z.B. Salz)     <--- kalibrieren
  1.5,  // Pos2  (z.B. Pfeffer)  <--- kalibrieren
  1.2,  // Pos3  (z.B. Rosmarin) <--- kalibrieren
  1.8,  // Pos4  (z.B. Paprika)  <--- kalibrieren
  // ggf. erweitern
};

// ------------------- Zustände -------------------
static long long abs_steps = 0;
static int current_index   = 0;
static int pos_count       = POS_COUNT_DEFAULT;

static char  spices_map[MAX_POS][SPICE_NAME_MAX];
static float dose_factor_per_pos[MAX_POS];  // Feintuning je Position (1.0 = neutral)

// ------------------- Servo-State -------------------
static Servo servo;
static uint32_t servo_pos_us = SERVO_BACK_US;

// ------------------- Debounce (Buttons) -------------------
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

// ---------- Button-Monitor / Logging ----------
struct BtnMon {
  uint8_t pin;
  const char* name;
  deb_state_t* db;
  uint8_t prev;                // letzter stabiler Level
  unsigned long t_change_ms;   // Zeitstempel der letzten Änderung
  bool long_reported;          // longpress schon gemeldet?
};

static BtnMon BM_STEP  { BTN_STEP,  "STEP",  &db_cycle,  HIGH, 0, false };
static BtnMon BM_SERVO { BTN_SERVO, "SERVO", &db_servo,  HIGH, 0, false };

// LOW = pressed (wegen INPUT_PULLUP)
static void monitor_button(BtnMon& b) {
  int level = debounce_read(b.pin, b.db);
  if (level != b.prev) {
    b.prev = level;
    b.t_change_ms = millis();
    b.long_reported = false;
    const char* evt = (level == LOW) ? "pressed" : "released";
    Serial.printf("{\"btn\":\"%s\",\"event\":\"%s\",\"pin\":%u,\"level\":%u,\"ms\":%lu}\n",
                  b.name, evt, b.pin, level, b.t_change_ms);
  } else {
    // Long-Press Meldung nach 800 ms halten
    if (level == LOW && !b.long_reported) {
      unsigned long dt = millis() - b.t_change_ms;
      if (dt >= 800) {
        b.long_reported = true;
        Serial.printf("{\"btn\":\"%s\",\"event\":\"longpress\",\"duration_ms\":%lu}\n",
                      b.name, dt);
      }
    }
  }
}

// ------------------- WLAN -------------------
void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PW);
  uint32_t t0 = millis();
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] OK  IP="); Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WIFI] FAILED");
  }
}

// ------------------- Stepper-Helpers -------------------
static inline void step_pulse_once(){
  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(STEP_PULSE_HIGH_US);
  digitalWrite(STEP_PIN, LOW);
}
static inline void set_dir(int lvl){
  digitalWrite(DIR_PIN, lvl ? HIGH : LOW);
  delayMicroseconds(5);
}
static void move_steps_ramped(int delta_steps, const speed_profile_t *p){
  if(delta_steps == 0 || !p) return;
  int us_min = (p->us_min < 200) ? 200 : p->us_min;
  int us_max = (p->us_max < us_min) ? us_min : p->us_max;
  int d_us   = (p->accel_per_step_us < 1) ? 1 : p->accel_per_step_us;

  int dir  = (delta_steps>0) ? 1 : 0;
  int cnt  = (delta_steps>0) ?  delta_steps : -delta_steps;

  set_dir(dir);

  int accel_steps = (us_max - us_min + d_us - 1) / d_us;
  int decel_steps = accel_steps;
  if (2*accel_steps > cnt) { accel_steps = cnt / 2; decel_steps = cnt - accel_steps; }
  int plateau_steps = cnt - accel_steps - decel_steps;

  int us = us_max;
  for (int i=0; i<accel_steps; ++i){ step_pulse_once(); delayMicroseconds(us); abs_steps += (dir? +1 : -1); us -= d_us; if (us < us_min) us = us_min; }
  for (int i=0; i<plateau_steps; ++i){ step_pulse_once(); delayMicroseconds(us); abs_steps += (dir? +1 : -1); }
  for (int i=0; i<decel_steps; ++i){ step_pulse_once(); delayMicroseconds(us); abs_steps += (dir? +1 : -1); us += d_us; if (us > us_max) us = us_max; }
}

// ------------------- Servo-Helpers -------------------
static void servo_write_us(uint32_t us){ servo.writeMicroseconds((int)us); }
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

// ------------------- Positionierung -------------------
static void goto_index(int target_idx){
  if(target_idx < 0) target_idx = 0;
  if(target_idx >= pos_count) target_idx = pos_count - 1;
  int delta_idx   = target_idx - current_index;
  int delta_steps = delta_idx * TRAVEL_STEPS_PER_STOP;
  Serial.printf("[MOVE] Pos%d -> Pos%d  steps=%d\n", current_index+1, target_idx+1, delta_steps);
  move_steps_ramped(delta_steps, &PROFILE_MOVE);
  current_index = target_idx;
}
static void dispense_rotations(float rotations){
  if (rotations <= 0.f) return;
  int steps = (int)(rotations * (float)DISPENSE_STEPS_PER_REV + 0.5f);
  Serial.printf("[DISP] @Pos%d: rotations=%.3f -> steps=%d\n", current_index+1, rotations, steps);
  move_steps_ramped(steps, &PROFILE_DISP);
}

// ------------------- Mapping Gewürzname ↔ Position -------------------
static void spice_clear_all(void){ for(int i=0;i<MAX_POS;i++) spices_map[i][0]='\0'; }
static void lowercase_inplace(char *s){ for(size_t i=0; s && s[i]; ++i) s[i]=(char)tolower((unsigned char)s[i]); }
static void spice_set(int idx, const char *name){
  if(idx<0 || idx>=MAX_POS || !name) return;
  strncpy(spices_map[idx], name, SPICE_NAME_MAX-1);
  spices_map[idx][SPICE_NAME_MAX-1]='\0';
  lowercase_inplace(spices_map[idx]);
}
static int find_pos_by_name(const char *name){
  if(!name) return -1;
  char tmp[SPICE_NAME_MAX];
  strncpy(tmp, name, SPICE_NAME_MAX-1);
  tmp[SPICE_NAME_MAX-1]='\0';
  lowercase_inplace(tmp);
  for(int i=0;i<pos_count;i++){
    if(spices_map[i][0]=='\0') continue;
    if(strcmp(spices_map[i], tmp)==0) return i; // beide lowercased -> strcmp reicht
  }
  return -1;
}

// ------------------- JSON → Targets (Gramm) -------------------
typedef struct { int idx; float grams; } target_grams_t;

static int build_targets_with_grams(JsonArray spices, target_grams_t *out, int out_max){
  if(out_max<=0) return 0;
  float grams_per_pos[MAX_POS]={0};
  bool seen[MAX_POS]={0};
  int order[MAX_POS]; int order_n=0;

  for(JsonObject sp : spices){
    const char* name = sp["name"];
    float grams = sp["grams"] | 0.0f;
    if(!name || grams<=0) continue;
    int pos = find_pos_by_name(name);
    if(pos<0){ Serial.printf("  - fehlt: '%s'\n", name); continue; }
    grams_per_pos[pos] += grams;
    if(!seen[pos]){ seen[pos]=true; order[order_n++]=pos; }
    Serial.printf("  + match: '%s' -> Pos%d (+%.3f g) = %.3f g\n", name, pos+1, grams, grams_per_pos[pos]);
  }
  int n=0;
  for(int k=0;k<order_n && n<out_max;k++){
    int p = order[k];
    if(grams_per_pos[p]>0) out[n++] = (target_grams_t){ p, grams_per_pos[p] };
  }
  Serial.printf("[MATCH] targets=%d\n", n);
  return n;
}

static void run_cycle_targets_grams(uint32_t *p_servo_pos, const target_grams_t *tg, int n){
  if(!p_servo_pos || !tg || n<=0) return;
  if(pos_count < 1) pos_count = 1;
  if(pos_count > MAX_POS) pos_count = MAX_POS;

  *p_servo_pos = servo_to_front(*p_servo_pos);
  delay(PAUSE_AFTER_COUPLE);

  for(int i=0;i<n;i++){
    int idx = tg[i].idx;
    float grams = tg[i].grams;
    if(idx<0 || idx>=pos_count || grams<=0) continue;

    goto_index(idx);
    delay(PAUSE_BEFORE_MOVE);

    *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);

    float gpr = (GRAMS_PER_ROTATION[idx] > 0.001f) ? GRAMS_PER_ROTATION[idx] : 1.0f;
    float rotations = grams / gpr;
    rotations *= dose_factor_per_pos[idx]; // Feintuning optional

    Serial.printf("[DOSE] Pos%d: grams=%.3f, g/rot=%.3f -> rot=%.3f\n",
                  idx+1, grams, gpr, rotations);
    dispense_rotations(rotations);
    delay(PAUSE_AFTER_DISP);

    *p_servo_pos = servo_to_front(*p_servo_pos);
    delay(PAUSE_AFTER_COUPLE);
  }

  goto_index(0);
  *p_servo_pos = decouple_to_back_with_pause(*p_servo_pos);
  Serial.println("[DONE] Pos1 erreicht, Servo hinten (Park/Start).");
}

// ------------------- AI-Aufruf (HTTP) -------------------
bool fetch_plan_and_run(const String& dish, int servings, const char* intensity) {
  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("{\"error\":\"wifi_not_connected\"}");
    return false;
  }

  JsonDocument req;                // v7: elastisch, Kapazität automatisch
  req["dish"] = dish;
  req["servings"] = servings;
  req["intensity"] = intensity;
  String body; serializeJson(req, body);


  HTTPClient http;
  http.begin(AI_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code != 200) {
    Serial.printf("{\"error\":\"http\",\"code\":%d}\n", code);
    http.end();
    return false;
  }

  String resp = http.getString();
  http.end();

  JsonDocument doc;
  auto err = deserializeJson(doc, resp);
  if (err) {
    Serial.println("{\"error\":\"invalid_ai_json\"}");
    return false;
  }

  JsonArray spices = doc["spices"].as<JsonArray>();
  if (spices.isNull() || spices.size()==0) {
    Serial.println("{\"error\":\"no_spices_from_ai\"}");
    return false;
  }

  target_grams_t targets[MAX_POS];
  int n = build_targets_with_grams(spices, targets, MAX_POS);
  if (n <= 0) {
    Serial.println("{\"status\":\"noop\"}");
    return false;
  }

  run_cycle_targets_grams(&servo_pos_us, targets, n);
  Serial.println("{\"status\":\"done\"}");
  return true;
}

// ------------------- Setup / Loop -------------------
static String serialCmd;

void setup(){
  Serial.begin(115200);
  delay(150);

  // Pins
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN,  OUTPUT);
  pinMode(EN_PIN,   OUTPUT);
  digitalWrite(EN_PIN, LOW);  // Enable aktiv (LOW)

  pinMode(BTN_STEP,  INPUT_PULLUP);
  pinMode(BTN_SERVO, INPUT_PULLUP);

  // Servo
  servo.setPeriodHertz(SERVO_FREQ_HZ);
  servo.attach(SERVO_PIN, 500, 2500);
  servo_pos_us = SERVO_BACK_US;
  servo.writeMicroseconds((int)servo_pos_us);

  // Grundzustand
  pos_count = POS_COUNT_DEFAULT;
  current_index = 0;
  abs_steps = 0;

  for(int i=0;i<MAX_POS;i++) dose_factor_per_pos[i]=1.0f;
  spice_clear_all();
  // --- Regal-Layout (Name -> Position) HIER ANPASSEN ---
  spice_set(0, "salz");
  spice_set(1, "pfeffer");
  spice_set(2, "rosmarin");
  spice_set(3, "paprika");

  // Button-Init-Logs
  // (nach pinMode, damit Pegel stabil gelesen werden)
  extern BtnMon BM_STEP, BM_SERVO;
  BM_STEP.prev  = digitalRead(BTN_STEP);
  BM_SERVO.prev = digitalRead(BTN_SERVO);
  BM_STEP.t_change_ms  = millis();
  BM_SERVO.t_change_ms = millis();
  Serial.printf("{\"btn\":\"%s\",\"mode\":\"INPUT_PULLUP\",\"pin\":%d,\"initial_level\":%d}\n",
                BM_STEP.name,  BM_STEP.pin,  BM_STEP.prev);
  Serial.printf("{\"btn\":\"%s\",\"mode\":\"INPUT_PULLUP\",\"pin\":%d,\"initial_level\":%d}\n",
                BM_SERVO.name, BM_SERVO.pin, BM_SERVO.prev);

  // WLAN verbinden
  wifiConnect();
  Serial.println("{\"status\":\"ready\"}");
}

void loop(){
  // ---- Laufender Button-Monitor (spamfrei: nur bei Änderungen/Events) ----
  monitor_button(BM_STEP);
  monitor_button(BM_SERVO);

  // ---- Aktionen auf entprellte Fallende Flanke ----
  if (edge_falling(BTN_SERVO, &db_servo)) {
    Serial.println("{\"btn\":\"SERVO\",\"event\":\"edge_falling\",\"action\":\"servo_toggle\"}");
    if (servo_pos_us == SERVO_BACK_US) servo_pos_us = servo_to_front(servo_pos_us);
    else                               servo_pos_us = decouple_to_back_with_pause(servo_pos_us);
  }

  if (edge_falling(BTN_STEP, &db_cycle)) {
    Serial.println("{\"btn\":\"STEP\",\"event\":\"edge_falling\",\"action\":\"fetch_plan\"}");
    Serial.println("[UI] Anfrage an AI: chili con carne");
    fetch_plan_and_run("chili con carne", 2, "medium");   // <--- später durch Displayeingabe ersetzen
  }

  // ---- Optional: Serial-Kommando 'dish: <text>' ----
  while (Serial.available()){
    char c=(char)Serial.read();
    if(c=='\n'){
      serialCmd.trim();
      if(serialCmd.startsWith("dish:")){
        String dish = serialCmd.substring(5); dish.trim();
        if(dish.length()>0){
          Serial.printf("[SERIAL] dish='%s'\n", dish.c_str());
          fetch_plan_and_run(dish, 2, "medium");
        }
      }
      serialCmd="";
    } else if (c!='\r'){
      serialCmd += c;
      if(serialCmd.length()>512) serialCmd="";
    }
  }

  delay(5);
}
