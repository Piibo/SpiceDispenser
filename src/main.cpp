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

// ---------------- Button Debounce/Edges ----------------
struct DebState { uint8_t last_level{1}; uint8_t stable_level{1}; uint8_t stable_count{0}; };
static DebState db_sel, db_servo;

static bool voice_busy = false;
static bool sel_armed = false;
static unsigned long sel_down_ms = 0;
static constexpr unsigned LONG_PRESS_MS = 800;

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

// Robust: direkt aus dem JsonDocument lesen + starke Logs.
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
      if (sp.isNull()) {
        Serial.printf("  [CONV] [%u] not an object -> skip\n", i++);
        continue;
      }

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

// ---------------- Rotary: eine Rastung = eine Aktion ----------------
#define ENCODER_COUNTS_PER_DETENT 2

static int readEncoderNotches(){
  static bool inited = false;
  static uint8_t prev = 0;
  static int8_t accum = 0;

  uint8_t curr = (digitalRead(ROT_CLK) ? 2 : 0) | (digitalRead(ROT_DT) ? 1 : 0);
  if (!inited) { prev = curr; inited = true; return 0; }

  static const int8_t dir_table[16] = {
    0, -1, +1, 0,
    +1, 0,  0, -1,
    -1, 0,  0, +1,
    0, +1, -1, 0
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
    JsonArrayConst spicesDbg = doc["spices"].as<JsonArrayConst>();
    Serial.printf("[VOICE] title='%s', spices.isNull=%d, spices.size=%u\n",
                  title, spicesDbg.isNull()?1:0, (unsigned)(spicesDbg.isNull()?0:spicesDbg.size()));

    std::vector<UIRecipe> uiRecipes;
    docToUIRecipes(doc, title, uiRecipes);

    if (!uiRecipes.empty()) {
      Serial.printf("[VOICE] UI recipes now=%u, first count=%u\n",
                    (unsigned)uiRecipes.size(), (unsigned)uiRecipes[0].spices.size());
      for (size_t k=0; k<uiRecipes[0].spices.size(); ++k) {
        Serial.printf("  [VOICE/UI] %u: '%s' -> %.3f g\n", (unsigned)k,
                      uiRecipes[0].spices[k].name.c_str(),
                      uiRecipes[0].spices[k].amount);
      }
      ui_showAIResult(uiRecipes);
    } else {
      Serial.println("[VOICE] UI recipes STILL 0 -> show error");
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
    JsonArrayConst spicesDbg = doc["spices"].as<JsonArrayConst>();
    Serial.printf("[PLAN] title='%s', spices.isNull=%d, spices.size=%u\n",
                  title, spicesDbg.isNull()?1:0, (unsigned)(spicesDbg.isNull()?0:spicesDbg.size()));

    std::vector<UIRecipe> uiRecipes;
    docToUIRecipes(doc, title, uiRecipes);

    if (!uiRecipes.empty()) {
      Serial.printf("[PLAN] UI recipes now=%u, first count=%u\n",
                    (unsigned)uiRecipes.size(), (unsigned)uiRecipes[0].spices.size());
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
  // Manuelles "Rezept" aus den belegten Dosen bauen
  UIRecipe manual;
  manual.name = "Einzel-Auswahl";

  for (int i = 0; i < pos_count; ++i) {
    if (spices_map[i][0] == '\0') continue;         // Slot leer
    manual.spices.push_back(UISpice{ spices_map[i], 0.0 }); // Startmenge 0 g
  }

  if (manual.spices.empty()) {
    ui_showAIError("Keine Gewuerze konfiguriert");
    return false;
  }

  std::vector<UIRecipe> list;
  list.push_back(std::move(manual));
  ui_showAIResult(list);   // öffnet DETAIL-Screen mit Weiter/Zurueck + EDIT
  return true;
}

// Deprecation-frei: Name aus AI-Doc holen
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
  if (doc["dish"].is<String>()) {
    String n = doc["dish"].as<String>(); n.trim();
    if (n.length() > 0) return n;
  }
  if (doc["title"].is<String>()) {
    String n = doc["title"].as<String>(); n.trim();
    if (n.length() > 0) return n;
  }
  return "";
}

// Spracheingabe starten und den selektierten Spice-Namen (nur Name, nicht Gramm)
// sowie den Slot in spices_map auf den neuen Namen setzen. Danach zurück in DETAIL.
static void renameSelectedSpiceByVoice() {
  // Vor Voice den aktuellen Namen der selektierten Zeile merken; wenn Auswahl auf Weiter/Zurück -> abort
  char oldNameBuf[SPICE_NAME_MAX] = {0};
  bool haveOld = ui_getSelectedSpiceName(oldNameBuf, sizeof(oldNameBuf));
  if (!haveOld || strlen(oldNameBuf) == 0) {
    ui_showDetail();
    return;
  }

  // Klassische Voice-Screens zeigen
  ui_renderVoiceInputScreen();
  auto onSending = [](){ ui_renderVoiceSendRequestScreen(); };

  JsonDocument doc;
  if (ai_post_voice("de", 1, "medium", doc, onSending)) {
    String name = extract_spice_name_from_doc(doc);
    name.trim(); name.toLowerCase();

    if (name.length() > 0) {
      // 1) UI-Zeile umbenennen
      ui_renameSelectedSpice(name.c_str());
      // 2) Slot im spices_map anpassen (falls alter Slot gefunden)
      int oldPos = find_pos_by_name(oldNameBuf);
      if (oldPos >= 0) {
        spice_set(oldPos, name.c_str());
        Serial.printf("[VOICE] Slot %d umbenannt: '%s' -> '%s'\n", oldPos+1, oldNameBuf, name.c_str());
      } else {
        Serial.printf("[VOICE] alter Slot fuer '%s' nicht gefunden (AI-Resultat-Mode?)\n", oldNameBuf);
      }
    } else {
      Serial.println("[VOICE] kein Name erkannt");
    }
  } else {
    Serial.println("[VOICE] ai_post_voice FAILED");
  }

  // Immer zurück zur Einzel-Auswahl (DETAIL)
  ui_showDetail();
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
  // Layout anpassen:
  spice_set(0, "salz");
  spice_set(1, "pfeffer");
  spice_set(2, "rosmarin");
  spice_set(3, "paprika");

  db_sel.last_level   = db_sel.stable_level   = digitalRead(BTN_SEL);
  db_servo.last_level = db_servo.stable_level = digitalRead(BTN_SERVO);

  ui_init();

  wifi_connect();
  ai_health();

  Serial.println("{\"status\":\"ready\"}");
}

void loop(){
  // Encoder -> UI: eine Rastung = eine Aktion
  int notches = readEncoderNotches();
  if (notches != 0) {
    UIState st = ui_getState();
    if (st.screen == UIScreen::EDIT) {
      ui_nudgeAmount(notches);       // im EDIT: Menge ändern
    } else {
      ui_nudgeSelection(notches);    // sonst: Auswahl bewegen
    }
    Serial.printf("[ENC] notch=%d (screen=%d)\n", notches, (int)st.screen);
  }

  // SERVO-Toggle
  if (edge_falling(BTN_SERVO, &db_servo)) {
    Serial.println("{\"btn\":\"SERVO\",\"event\":\"edge_falling\",\"action\":\"servo_toggle\"}");
    if (servo_pos_us == SERVO_BACK_US) servo_pos_us = mechServoToFront(servo_pos_us);
    else                               servo_pos_us = mechDecoupleBack(servo_pos_us);
  }

  // SEL press-duration handling (falling-only + Dauer)
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
        UIState st2 = ui_getState(); // 0=AI, 1=Einzel
        if (st2.selected == 0) {
          // AI-Gerichtserkennung
          if (dur >= LONG_PRESS_MS) {
            startFixedPlanAndShow("chili con carne");
          } else {
            startVoiceAndShowResult();
          }
        } else {
          // EINZEL-GEWÜRZ: Liste anzeigen + EDIT möglich
          showManualSpiceSelection();
        }
        voice_busy = false;
      }
    } else {
      // DETAIL: langer Druck = Gewuerz per Voice umbenennen (nur wenn auf einer Gewuerzzeile)
      UIState st2 = ui_getState();
      if (st2.screen == UIScreen::DETAIL && dur >= LONG_PRESS_MS) {
        if (!voice_busy) {
          voice_busy = true;
          renameSelectedSpiceByVoice();
          voice_busy = false;
        }
      } else {
        // kurzer Klick: normales UI-Verhalten (Weiter/Zurueck/Edit)
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
            if (!uiRecipes.empty()) ui_showAIResult(uiRecipes);
            else ui_showAIError("Keine Gewuerze erkannt");
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

  // Nach „OK/Weiter“ aus der UI dosieren
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
    } else {
      Serial.println("{\"status\":\"noop\"}");
    }
  }

  delay(5);
}
