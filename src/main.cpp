#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <ctype.h>

#include "pins.h"
#include "config.h"
#include "mech.h"
#include "ai.h"

// --- Button Debounce/Edges (leichtgewichtig) ---
struct DebState { uint8_t last_level{1}; uint8_t stable_level{1}; uint8_t stable_count{0}; };
static DebState db_cycle, db_servo;

static int debounce_read(uint8_t pin, DebState* st){
  int level = digitalRead(pin);
  if(level == st->last_level){ if(st->stable_count<5) st->stable_count++; }
  else { st->stable_count=0; st->last_level=level; }
  if(st->stable_count>=5) st->stable_level=level;
  return st->stable_level;
}
static bool edge_falling(uint8_t pin, DebState* st){
  static uint8_t prev[64]={0};
  int val = debounce_read(pin,st);
  bool falling = (prev[pin]==HIGH && val==LOW);
  prev[pin]=val; return falling;
}
static bool edge_rising(uint8_t pin, DebState* st){
  static uint8_t prev_up[64]={0};
  int val = debounce_read(pin,st);
  bool rising = (prev_up[pin]==LOW && val==HIGH);
  prev_up[pin]=val; return rising;
}

// --- Spice-Mapping
static char  spices_map[MAX_POS][SPICE_NAME_MAX];
static int   pos_count = POS_COUNT_DEFAULT;

static void spice_clear_all(){ for(int i=0;i<MAX_POS;i++) spices_map[i][0]='\0'; }
static void lowercase_inplace(char *s){ for(size_t i=0; s && s[i]; ++i) s[i]=(char)tolower((unsigned char)s[i]); }
static void spice_set(int idx, const char *name){
  if(idx<0 || idx>=MAX_POS || !name) return;
  strncpy(spices_map[idx], name, SPICE_NAME_MAX-1);
  spices_map[idx][SPICE_NAME_MAX-1]='\0'; lowercase_inplace(spices_map[idx]);
}
static int find_pos_by_name(const char *name){
  if(!name) return -1; char tmp[SPICE_NAME_MAX];
  strncpy(tmp, name, SPICE_NAME_MAX-1); tmp[SPICE_NAME_MAX-1]='\0'; lowercase_inplace(tmp);
  for(int i=0;i<pos_count;i++){ if(spices_map[i][0]=='\0') continue; if(strcmp(spices_map[i], tmp)==0) return i; }
  return -1;
}

// --- Targets aus JSON (Gramm) ---
struct TargetGrams { int idx; float grams; };

static int build_targets_with_grams(JsonArray spices, TargetGrams* out, int out_max){
  if(out_max<=0) return 0;
  float grams_per_pos[MAX_POS]={0}; bool seen[MAX_POS]={0}; int order[MAX_POS]; int order_n=0;
  for(JsonObject sp : spices){
    const char* name = sp["name"]; float grams = sp["grams"] | 0.0f;
    if(!name || grams<=0) continue;
    int pos = find_pos_by_name(name);
    if(pos<0){ Serial.printf("  - fehlt: '%s'\n", name); continue; }
    grams_per_pos[pos] += grams;
    if(!seen[pos]){ seen[pos]=true; order[order_n++]=pos; }
    Serial.printf("  + match: '%s' -> Pos%d (+%.3f g) = %.3f g\n", name, pos+1, grams, grams_per_pos[pos]);
  }
  int n=0; for(int k=0;k<order_n && n<out_max;k++){ int p = order[k]; if(grams_per_pos[p]>0) out[n++] = TargetGrams{ p, grams_per_pos[p] }; }
  Serial.printf("[MATCH] targets=%d\n", n); return n;
}

static void run_cycle_targets_grams(uint32_t& servo_pos_us, const TargetGrams* tg, int n){
  if(!tg || n<=0) return;

  servo_pos_us = mechServoToFront(servo_pos_us); delay(PAUSE_AFTER_COUPLE);

  for(int i=0;i<n;i++){
    int idx = tg[i].idx; float grams = tg[i].grams;
    if(idx<0 || idx>=pos_count || grams<=0) continue;
    mechGotoIndex(idx); delay(PAUSE_BEFORE_MOVE);

    servo_pos_us = mechDecoupleBack(servo_pos_us); delay(PAUSE_AFTER_COUPLE);

    float gpr = (GRAMS_PER_ROTATION[idx] > 0.001f) ? GRAMS_PER_ROTATION[idx] : 1.0f;
    float rotations = (grams / gpr); // optional: * Faktor pro Position
    Serial.printf("[DOSE] Pos%d: grams=%.3f, g/rot=%.3f -> rot=%.3f\n", idx+1, grams, gpr, rotations);

    mechDispenseRotations(rotations); delay(PAUSE_AFTER_DISP);
    servo_pos_us = mechServoToFront(servo_pos_us); delay(PAUSE_AFTER_COUPLE);
  }

  mechGotoIndex(0);
  servo_pos_us = mechDecoupleBack(servo_pos_us);
  Serial.println("[DONE] Pos1 erreicht, Servo hinten (Park/Start).");
}

// --- Serial-Kommandopuffer ---
static String serialCmd;
static uint32_t servo_pos_us = SERVO_BACK_US;

void setup(){
  Serial.begin(115200); delay(150);

  pinMode(BTN_STEP,  INPUT_PULLUP);
  pinMode(BTN_SERVO, INPUT_PULLUP);

  mechInit();
  mechSetPosCount(POS_COUNT_DEFAULT);
  pos_count = POS_COUNT_DEFAULT;

  spice_clear_all();
  // Layout anpassen:
  spice_set(0, "salz");
  spice_set(1, "pfeffer");
  spice_set(2, "rosmarin");
  spice_set(3, "paprika");

  // Initiale Buttonlevels merken
  db_cycle.last_level = db_cycle.stable_level = digitalRead(BTN_STEP);
  db_servo.last_level = db_servo.stable_level = digitalRead(BTN_SERVO);

  wifi_connect();
  ai_health();
  Serial.println("{\"status\":\"ready\"}");
}

void loop(){
  // SERVO-Toggle
  if (edge_falling(BTN_SERVO, &db_servo)) {
    Serial.println("{\"btn\":\"SERVO\",\"event\":\"edge_falling\",\"action\":\"servo_toggle\"}");
    if (servo_pos_us == SERVO_BACK_US) servo_pos_us = mechServoToFront(servo_pos_us);
    else                               servo_pos_us = mechDecoupleBack(servo_pos_us);
  }

  // STEP: Kurz/ Lang
  static unsigned long step_down_ms = 0;
  if (edge_falling(BTN_STEP, &db_cycle)) {
    step_down_ms = millis();
    Serial.println("{\"btn\":\"STEP\",\"event\":\"edge_falling\",\"action\":\"arm\"}");
  }
  if (edge_rising(BTN_STEP, &db_cycle)) {
    unsigned long dur = millis() - step_down_ms;
    Serial.printf("{\"btn\":\"STEP\",\"event\":\"released\",\"press_ms\":%lu}\n", dur);
    JsonDocument doc;
    if (dur >= 800) {
      Serial.println("[UI] Sprachmodus: Bitte am PC sprechen …");
      if (ai_post_voice("de", 2, "medium", doc)) {
        JsonArray spices = doc["spices"].as<JsonArray>();
        if (!spices.isNull() && spices.size()>0) {
          TargetGrams tg[MAX_POS]; int n = build_targets_with_grams(spices, tg, MAX_POS);
          run_cycle_targets_grams(servo_pos_us, tg, n);
        } else Serial.println("{\"error\":\"no_spices_from_ai\"}");
      }
    } else {
      Serial.println("[UI] Anfrage an AI: chili con carne");
      if (ai_post_plan("chili con carne", 2, "medium", doc)) {
        JsonArray spices = doc["spices"].as<JsonArray>();
        if (!spices.isNull() && spices.size()>0) {
          TargetGrams tg[MAX_POS]; int n = build_targets_with_grams(spices, tg, MAX_POS);
          run_cycle_targets_grams(servo_pos_us, tg, n);
        } else Serial.println("{\"error\":\"no_spices_from_ai\"}");
      }
    }
  }

  // Optional: Serial 'dish: <text>'
  while (Serial.available()){
    char c=(char)Serial.read();
    if(c=='\n'){
      serialCmd.trim();
      if(serialCmd.startsWith("dish:")){
        String dish = serialCmd.substring(5); dish.trim();
        if(dish.length()>0){
          Serial.printf("[SERIAL] dish='%s'\n", dish.c_str());
          JsonDocument doc;
          if (ai_post_plan(dish, 2, "medium", doc)) {
            JsonArray spices = doc["spices"].as<JsonArray>();
            if (!spices.isNull() && spices.size()>0) {
              TargetGrams tg[MAX_POS]; int n = build_targets_with_grams(spices, tg, MAX_POS);
              run_cycle_targets_grams(servo_pos_us, tg, n);
            } else Serial.println("{\"error\":\"no_spices_from_ai\"}");
          }
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
