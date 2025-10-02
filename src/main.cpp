#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <ctype.h>
#include <vector>
#include <string>

#include "pins.h"
#include "config.h"
#include "mech.h"
#include "ai.h"
#include "ui.h"
#include "lexicon.h"

// ---------- Forward Declarations (Lexikon/Matching) ----------
static String normalize_spice(const String& inRaw);
static int    levenshtein(const String& a, const String& b);
static bool   findCanonicalSpice(const String& rawIn, String& canonOut);

// ---------------- Button Debounce/Edges ----------------
struct DebState { uint8_t last_level{1}; uint8_t stable_level{1}; uint8_t stable_count{0}; };
static DebState db_sel, db_servo;

static bool voice_busy = false;
static bool sel_armed  = false;
static unsigned long sel_down_ms = 0;
static constexpr unsigned LONG_PRESS_MS = 800;

// Merker: Sind wir in der Einzelliste?
static bool s_isManualList = false;

// Debounce + Edges
static int debounce_read(uint8_t pin, DebState* st){
  int level = digitalRead(pin);
  if(level == st->last_level){ if(st->stable_count<5) st->stable_count++; }
  else { st->stable_count=0; st->last_level=level; }
  if(st->stable_count>=5) st->stable_level=level;
  return st->stable_level;
}
static bool edge_falling(uint8_t pin, DebState* st){
  static uint8_t prev[64] = {0};
  static bool inited[64]  = {false};
  int val = debounce_read(pin, st);
  if (!inited[pin]) { prev[pin] = val; inited[pin] = true; }
  bool falling = (prev[pin]==HIGH && val==LOW);
  prev[pin] = val;
  return falling;
}

// ---------------- Spice-Mapping ----------------
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

// ---------------- Targets/Umrechnung ----------------
struct TargetGrams { int idx; float grams; };

// Robust: direkt aus dem JsonDocument lesen + Logs.
static void docToUIRecipes(const JsonDocument& doc, const char* title, std::vector<UIRecipe>& out) {
  JsonArrayConst arr = doc["spices"].as<JsonArrayConst>();
  const char* ttl = (title && *title) ? title : (doc["title"] | doc["dish"] | "AI-Rezept");
  Serial.printf("[CONV] docToUIRecipes: title='%s', spices.isNull=%d, size=%u\n",
                ttl, arr.isNull()?1:0, (unsigned)(arr.isNull()?0:arr.size()));

  out.clear();
  UIRecipe r;
  r.name = ttl ? ttl : "AI-Rezept";

  if (!arr.isNull()) {
    uint16_t i = 0;
    for (JsonVariantConst v : arr) {
      JsonObjectConst sp = v.as<JsonObjectConst>();
      if (sp.isNull()) { Serial.printf("  [CONV] [%u] not an object -> skip\n", i++); continue; }

      String nameS = sp["name"].as<String>();
      double grams = 0.0;

      if (sp["grams"].is<double>() || sp["grams"].is<float>() || sp["grams"].is<long>() || sp["grams"].is<int>()) {
        grams = sp["grams"].as<double>();
      } else {
        String gS = sp["grams"].as<String>();
        grams = gS.length() ? gS.toFloat() : 0.0;
      }

      Serial.printf("  [CONV] [%u] nameS='%s' grams=%.3f\n", i, nameS.c_str(), grams);

      if (nameS.length() > 0 && grams > 0.0) {
        r.spices.push_back(UISpice{ std::string(nameS.c_str()), grams });
        Serial.printf("  [CONV] [%u] -> added to UI list\n", i);
      } else {
        Serial.printf("  [CONV] [%u] skipped (invalid name/grams)\n", i);
      }
      ++i;
    }
  }

  if (!r.spices.empty()) {
    out.push_back(r);
    Serial.printf("[CONV] DONE -> out.size()=%u, first.count=%u\n",
                  (unsigned)out.size(), (unsigned)out[0].spices.size());
  } else {
    Serial.println("[CONV] DONE -> NO SPICES -> out.size()=0");
  }
}

// UI -> Mechanik-Targets
static int build_targets_from_ui(const UIRecipe& r, TargetGrams* out, int out_max){
  if(out_max<=0) return 0;
  float grams_per_pos[MAX_POS]={0}; bool seen[MAX_POS]={0}; int order[MAX_POS]; int order_n=0;

  for (const auto& s : r.spices) {
    if (s.amount <= 0) continue;
    int pos = find_pos_by_name(s.name.c_str());
    if (pos < 0) { Serial.printf("[MAP] fehlt: '%s'\n", s.name.c_str()); continue; }
    grams_per_pos[pos] += (float)s.amount;
    if (!seen[pos]) { seen[pos] = true; order[order_n++] = pos; }
    Serial.printf("[MAP] + '%s' -> Pos%d (+%.3f g) = %.3f g\n",
                  s.name.c_str(), pos+1, (float)s.amount, grams_per_pos[pos]);
  }

  int n=0; for(int k=0;k<order_n && n<out_max;k++){ int p = order[k]; if(grams_per_pos[p]>0) out[n++] = TargetGrams{ p, grams_per_pos[p] }; }
  Serial.printf("[MAP] targets=%d\n", n); return n;
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
    float rotations = (grams / gpr);
    Serial.printf("[DOSE] Pos%d: grams=%.3f, g/rot=%.3f -> rot=%.3f\n", idx+1, grams, gpr, rotations);

    mechDispenseRotations(rotations); delay(PAUSE_AFTER_DISP);
    servo_pos_us = mechServoToFront(servo_pos_us); delay(PAUSE_AFTER_COUPLE);
  }

  mechGotoIndex(0);
  servo_pos_us = mechDecoupleBack(servo_pos_us);
  Serial.println("[DONE] Pos1 erreicht, Servo hinten (Park/Start).");
}

// ---------------- Rotary ----------------
#define ENCODER_COUNTS_PER_DETENT 2
static int readEncoderNotches(){
  static bool inited = false;
  static uint8_t prev = 0;
  static int8_t accum = 0;

  uint8_t curr = (digitalRead(ROT_CLK) ? 2 : 0) | (digitalRead(ROT_DT) ? 1 : 0);
  if (!inited) { prev = curr; inited = true; return 0; }

  static const int8_t dir_table[16] = {
    0, -1, +1, 0,  +1, 0, 0, -1,  -1, 0, 0, +1,  0, +1, -1, 0
  };

  int8_t movement = dir_table[(prev << 2) | curr];
  prev = curr;

  if (movement) {
    accum += movement;
    if (accum >= ENCODER_COUNTS_PER_DETENT)  { accum = 0; return +1; }
    if (accum <= -ENCODER_COUNTS_PER_DETENT) { accum = 0; return -1; }
  }
  return 0;
}

// ---------------- Serial / Globals ----------------
static String serialCmd;
static uint32_t servo_pos_us = SERVO_BACK_US;

// ---------------- Voice & Fixed Plan Flows ----------------
// Gericht per Sprache: VOICE_INPUT -> (onSending) VOICE_SEND -> Ergebnisliste
static void startVoiceAndShowResult() {
  Serial.println("[VOICE] startVoiceAndShowResult()");
  ui_renderVoiceInputScreen();

  auto onSending = [](){
    Serial.println("[VOICE] VAD ended -> sending now");
    ui_renderVoiceSendRequestScreen();
  };

  JsonDocument doc;
  if (ai_post_voice("de", 2, "medium", doc, onSending)) {
    Serial.println("[VOICE] ai_post_voice OK");

    const char* title = doc["title"] | doc["dish"] | "AI-Rezept";
    std::vector<UIRecipe> uiRecipes;
    docToUIRecipes(doc, title, uiRecipes);

    if (!uiRecipes.empty()) {
      s_isManualList = false;
      ui_showAIResult(uiRecipes);
    } else {
      ui_showAIError("Keine Gewuerze erkannt");
    }
  } else {
    Serial.println("[VOICE] ai_post_voice FAILED");
    ui_showAIError("AI-Fehler");
  }
}

static void startFixedPlanAndShow(const char* dish) {
  Serial.printf("[PLAN] startFixedPlanAndShow dish='%s'\n", dish);
  auto onSending = [](){
    Serial.println("[PLAN] sending now");
    ui_renderVoiceSendRequestScreen();
  };

  JsonDocument doc;
  if (ai_post_plan(dish, 2, "medium", doc, onSending)) {
    Serial.println("[PLAN] ai_post_plan OK");

    const char* title = doc["title"] | doc["dish"] | dish;
    std::vector<UIRecipe> uiRecipes;
    docToUIRecipes(doc, title, uiRecipes);

    if (!uiRecipes.empty()) {
      s_isManualList = false;
      ui_showAIResult(uiRecipes);
    } else {
      ui_showAIError("Keine Gewuerze erkannt");
    }
  } else {
    Serial.println("[PLAN] ai_post_plan FAILED");
    ui_showAIError("AI-Fehler");
  }
}

static bool showManualSpiceSelection() {
  UIRecipe manual;
  manual.name = "Einzel-Auswahl";

  for (int i = 0; i < pos_count; ++i) {
    if (spices_map[i][0] == '\0') continue;
    manual.spices.push_back(UISpice{ spices_map[i], 0.0 });
  }

  if (manual.spices.empty()) {
    ui_showAIError("Keine Gewuerze konfiguriert");
    return false;
  }

  std::vector<UIRecipe> list;
  list.push_back(std::move(manual));
  s_isManualList = true;
  ui_showAIResult(list);
  return true;
}

// Name aus AI-Doc holen (spices[0].name -> dish -> title)
static String extract_spice_name_from_doc(const JsonDocument& doc) {
  JsonArrayConst arr = doc["spices"].as<JsonArrayConst>();
  if (!arr.isNull() && arr.size() > 0) {
    JsonObjectConst sp0 = arr[0].as<JsonObjectConst>();
    if (!sp0.isNull()) {
      String n = sp0["name"].as<String>();
      n.trim();
      if (n.length() > 0) return n;
    }
  }
  if (doc["dish"].is<String>())   { String n = doc["dish"].as<String>();  n.trim(); if (n.length() > 0) return n; }
  if (doc["title"].is<String>())  { String n = doc["title"].as<String>(); n.trim(); if (n.length() > 0) return n; }
  return "";
}

// --- Whitelist-/Fuzzy-Matching ---
static String normalize_spice(const String& inRaw) {
  String s = inRaw;
  s.trim(); s.toLowerCase();
  s.replace("ä","ae"); s.replace("ö","oe"); s.replace("ü","ue"); s.replace("ß","ss");
  String out; out.reserve(s.length());
  for (size_t i=0; i<s.length(); ++i) {
    char c = s.charAt(i);
    if ((c>='a'&&c<='z') || (c>='0'&&c<='9') || c==' ') out += c;
  }
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  return out;
}
static int levenshtein(const String& a, const String& b) {
  const int n = a.length(), m = b.length();
  if (n==0) return m;
  if (m==0) return n;
  std::vector<int> prev(m+1), curr(m+1);
  for (int j=0; j<=m; ++j) prev[j] = j;
  for (int i=1; i<=n; ++i) {
    curr[0] = i;
    for (int j=1; j<=m; ++j) {
      int cost = (a.charAt(i-1)==b.charAt(j-1)) ? 0 : 1;
      int del  = prev[j]   + 1;
      int ins  = curr[j-1] + 1;
      int sub  = prev[j-1] + cost;
      curr[j] = min(del, min(ins, sub));
    }
    prev.swap(curr);
  }
  return prev[m];
}
static bool findCanonicalSpice(const String& rawIn, String& canonOut) {
  String q = normalize_spice(rawIn);
  if (q.length()==0) return false;

  for (size_t i=0; i<SPICE_LEXICON_COUNT; ++i) {
    String cand = normalize_spice(String(SPICE_LEXICON[i]));
    if (cand == q) { canonOut = String(SPICE_LEXICON[i]); return true; }
  }
  for (size_t i=0; i<SPICE_LEXICON_COUNT; ++i) {
    String cand = normalize_spice(String(SPICE_LEXICON[i]));
    if (q.indexOf(cand) >= 0 || cand.indexOf(q) >= 0) { canonOut = String(SPICE_LEXICON[i]); return true; }
  }
  int bestIdx = -1, bestDist = 9999;
  for (size_t i=0; i<SPICE_LEXICON_COUNT; ++i) {
    String cand = normalize_spice(String(SPICE_LEXICON[i]));
    int d = levenshtein(q, cand);
    if (d < bestDist) { bestDist = d; bestIdx = (int)i; }
  }
  int limit = (q.length() <= 5) ? 1 : (q.length() <= 10 ? 2 : 3);
  if (bestIdx >= 0 && bestDist <= limit) { canonOut = String(SPICE_LEXICON[bestIdx]); return true; }
  return false;
}

// Voice-Umbenennen des selektierten Gewürzes (nur der Name); nur in Einzelliste
static void renameSelectedSpiceByVoice() {
  if (!s_isManualList) { ui_showDetail(); return; }

  char oldNameBuf[SPICE_NAME_MAX] = {0};
  if (!ui_getSelectedSpiceName(oldNameBuf, sizeof(oldNameBuf)) || strlen(oldNameBuf) == 0) {
    ui_showDetail();
    return;
  }

  ui_renderVoiceInputScreen(); // Aufnahme
  auto onSending = [](){ ui_renderVoiceSendRequestScreen(); }; // Senden

  JsonDocument doc;
  if (ai_post_voice("de", 1, "medium", doc, onSending)) {
    String name = extract_spice_name_from_doc(doc);
    name.trim();

    String canon;
    if (findCanonicalSpice(name, canon)) {
      ui_renameSelectedSpice(canon.c_str());
      int oldPos = find_pos_by_name(oldNameBuf);
      if (oldPos >= 0) { spice_set(oldPos, canon.c_str()); }
    } else {
      Serial.printf("[VOICE] '%s' ist kein erlaubtes Gewuerz -> abgelehnt\n", name.c_str());
    }
  } else {
    Serial.println("[VOICE] ai_post_voice FAILED");
  }

  ui_showDetail(); // zurück zur Liste
}

void setup(){
  Serial.begin(115200); delay(150);

  pinMode(BTN_SEL,   INPUT_PULLUP);
  pinMode(BTN_SERVO, INPUT_PULLUP);
  pinMode(ROT_CLK,   INPUT_PULLUP);
  pinMode(ROT_DT,    INPUT_PULLUP);

  mechInit();
  mechSetPosCount(POS_COUNT_DEFAULT);
  pos_count = POS_COUNT_DEFAULT;

  spice_clear_all();
  spice_set(0, "salz");
  spice_set(1, "pfeffer");
  spice_set(2, "rosmarin");
  spice_set(3, "paprika");
  spice_set(4, "curry");

  db_sel.last_level   = db_sel.stable_level   = digitalRead(BTN_SEL);
  db_servo.last_level = db_servo.stable_level = digitalRead(BTN_SERVO);

  ui_init();

  wifi_connect();
  ai_health();

  Serial.println("{\"status\":\"ready\"}");
}

void loop(){
  ui_tick(0);

  int notches = readEncoderNotches();
  if (notches != 0) {
    UIState st = ui_getState();
    if (st.screen == UIScreen::EDIT) ui_nudgeAmount(notches);
    else                             ui_nudgeSelection(notches);
    Serial.printf("[ENC] notch=%d (screen=%d)\n", notches, (int)st.screen);
  }

  if (edge_falling(BTN_SERVO, &db_servo)) {
    Serial.println("{\"btn\":\"SERVO\",\"event\":\"edge_falling\",\"action\":\"servo_toggle\"}");
    if (servo_pos_us == SERVO_BACK_US) servo_pos_us = mechServoToFront(servo_pos_us);
    else                               servo_pos_us = mechDecoupleBack(servo_pos_us);
  }

  if (edge_falling(BTN_SEL, &db_sel)) {
    sel_armed = true;
    sel_down_ms = millis();
  }
  if (sel_armed && digitalRead(BTN_SEL) == HIGH) {
    unsigned long dur = millis() - sel_down_ms;
    sel_armed = false;

    UIScreen scr = ui_getState().screen;

    if (scr == UIScreen::START) {
      if (!voice_busy) {
        voice_busy = true;
        UIState st2 = ui_getState();
        if (st2.selected == 0) {
          if (dur >= LONG_PRESS_MS) startFixedPlanAndShow("chili con carne");
          else                      startVoiceAndShowResult();
        } else {
          if (dur >= LONG_PRESS_MS) {
            if (servo_pos_us == SERVO_BACK_US) servo_pos_us = mechServoToFront(servo_pos_us);
            else                               servo_pos_us = mechDecoupleBack(servo_pos_us);
            Serial.println("[START] Einzel-Gewuerz: Servo toggled");
          } else {
            showManualSpiceSelection();
          }
        }
        voice_busy = false;
      }
    } else {
      UIState st2 = ui_getState();
      if (st2.screen == UIScreen::DETAIL && dur >= LONG_PRESS_MS && s_isManualList) {
        if (!voice_busy) {
          voice_busy = true;
          renameSelectedSpiceByVoice();
          voice_busy = false;
        }
      } else {
        ui_onBtnClick();
      }
    }
  }

  // Serial 'dish: <text>' (Test)
  while (Serial.available()){
    char c=(char)Serial.read();
    if(c=='\n'){
      serialCmd.trim();
      if(serialCmd.startsWith("dish:")){
        String dish = serialCmd.substring(5); dish.trim();
        if(dish.length()>0){
          Serial.printf("[SERIAL] dish='%s'\n", dish.c_str());
          JsonDocument doc;
          if (ai_post_plan(dish.c_str(), 2, "medium", doc)) {
            std::vector<UIRecipe> uiRecipes;
            const char* title = doc["title"] | doc["dish"] | "AI-Rezept";
            docToUIRecipes(doc, title, uiRecipes);
            if (!uiRecipes.empty()) {
              s_isManualList = false;
              ui_showAIResult(uiRecipes);
            } else ui_showAIError("Keine Gewuerze erkannt");
          } else {
            ui_showAIError("AI-Fehler");
          }
        }
      }
      serialCmd="";
    } else if (c!='\r'){
      serialCmd += c;
      if(serialCmd.length()>512) serialCmd="";
    }
  }

  // Nach „OK/Weiter“ dosieren
  UIRecipe edited;
  if (ui_takeEditedRecipe(edited)) {
    Serial.printf("[UI] confirm -> recipe='%s' items=%u\n",
                  edited.name.c_str(), (unsigned)edited.spices.size());
    for (size_t i=0;i<edited.spices.size();++i) {
      Serial.printf("  [UI] %u: '%s' -> %.3f g\n", (unsigned)i,
        edited.spices[i].name.c_str(), edited.spices[i].amount);
    }

    TargetGrams tg[MAX_POS];
    int n = build_targets_from_ui(edited, tg, MAX_POS);
    for (int i=0;i<n;i++) {
      Serial.printf("  [MAP] idx=%d grams=%.3f\n", tg[i].idx, tg[i].grams);
    }

    if (n > 0) {
      Serial.println("[MECH] start run_cycle_targets_grams()");
      run_cycle_targets_grams(servo_pos_us, tg, n);
      Serial.println("[MECH] end run_cycle_targets_grams()");
      ui_goStart();
      s_isManualList = false;
    } else {
      Serial.println("{\"status\":\"noop\"}");
    }
  }

  delay(5);
}
