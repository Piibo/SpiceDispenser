#include "ui.h"
#include "pins.h"
#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include "icons.h"
#include <cmath>

static std::vector<UIRecipe> g_recipes;
static UIScreen g_screen = UIScreen::START;
static int g_selected = 0;
static int g_detailTop = 0;
static int g_detailSel = 0;
static double g_potFiltered = 0.0;
static const float POT_ALPHA = 0.2f;
static String g_statusLine;
static bool g_hasPendingConfirm = false;
static UIRecipe g_pendingRecipe;
static bool g_voiceRequestPending = false;

static const uint8_t LINE_H = 10;
static const uint8_t TOP_MARGIN = 12;
static const uint8_t ITEMS_PER_PAGE = 4;
static const uint8_t TEXT_X = 4;
static const uint8_t TITLE_PAD_X = 4;

U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(
  U8G2_R0, OLED_CLK, OLED_MOSI, OLED_CS, OLED_DC, OLED_RST);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t recipeCount() {
  size_t n = g_recipes.size();
  return n > 255 ? 255 : static_cast<uint8_t>(n);
}

#ifndef POT_BITS
#define POT_BITS 4095
#endif

static int mapPotToRange(int potRaw, int maxExclusive) {
  if (maxExclusive <= 1) return 0;
  long v = potRaw;
  long idx = (v * maxExclusive) / (POT_BITS + 1);
  if (idx < 0) idx = 0;
  if (idx >= maxExclusive) idx = maxExclusive - 1;
  return (int)idx;
}

static double quantizeStepClamped(double x, double step, double maxv) {
  double q = floor(x / step + 0.5) * step;
  if (q < 0.0)  q = 0.0;
  if (q > maxv) q = maxv;
  return q;
}

static String fmtAmount(double v) {
  long iv = lround(v);
  if (fabs(v - iv) < 0.05) return String(iv);
  char buf[16];
  dtostrf(v, 0, 1, buf);
  return String(buf);
}

static void drawTitle(const char* title, bool /*showBack*/) {
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(TITLE_PAD_X, 10, title);
  u8g2.drawHLine(0, TOP_MARGIN - 1, 128);
  u8g2.setFont(FONT_TEXT);
}

static void drawStart() {
  u8g2.clearBuffer();
  drawTitle("Lass uns kochen!", false);

  auto cx = [](U8G2 &d, const char* s){ return (128 - (int)d.getStrWidth(s)) / 2; };

  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(cx(u8g2, "Knopf gedrueckt"), DISPLAY_HEIGHT/2 , "Knopf gedrueckt");
  u8g2.drawStr(cx(u8g2, "halten"), DISPLAY_HEIGHT/2 +10, "halten");

  u8g2.sendBuffer();
}

static void drawRecipeDetail(bool editing) {
  const UIRecipe& recipe = g_recipes[g_selected];
  const int total = (int)recipe.spices.size();

  // Reihenfolge: 0 = Weiter (oben), 1..total = Spices, total+1 = Weiter (unten), total+2 = Zurueck
  const int Nrows = total + 3;

  u8g2.clearBuffer();
  drawTitle(recipe.name.c_str(), true);

  if (total == 0) {
    u8g2.setFont(FONT_TEXT);
    u8g2.drawStr(TEXT_X, TOP_MARGIN + 8, "Keine Gewuerze erkannt");
    u8g2.sendBuffer();
    return;
  }

  g_detailTop = clampi(g_detailTop, 0, max(0, Nrows - (int)ITEMS_PER_PAGE));
  int start = g_detailTop;
  int endExclusive = min(start + (int)ITEMS_PER_PAGE, Nrows);

  for (int i = start; i < endExclusive; ++i) {
    int line = i - start;
    uint8_t y = TOP_MARGIN + 1 + line * LINE_H + 8;
    bool isSel = (i == g_detailSel);

    if (isSel) { u8g2.drawBox(0, TOP_MARGIN + line * LINE_H, 128, LINE_H); u8g2.setDrawColor(0); }
    else        { u8g2.setDrawColor(1); }

    if (i == 0 || i == total + 1) {
      // "Weiter" (oben UND unten)
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Weiter");
      drawIcon(IconType::Check, 118, y);
    } else if (i == total + 2) {
      // "Zurueck" (ganz unten)
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Zurueck");
      drawIcon(IconType::Left, 118, y);
    } else {
      // Gewuerz: Index in der Rezeptliste ist i-1
      const UISpice& sp = recipe.spices[i - 1];
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, sp.name.c_str());
      String amt = fmtAmount(sp.amount);
      u8g2_uint_t w = u8g2.getStrWidth(amt.c_str());
      u8g2.drawStr(128 - TITLE_PAD_X - w, y, amt.c_str());
    }

    if (isSel) u8g2.setDrawColor(1);
  }

  if (start > 0)                 drawIcon(IconType::ChevUp,   122, TOP_MARGIN + 8);
  if (endExclusive < Nrows)      drawIcon(IconType::ChevDown, 122, 64 - 2);

  u8g2.sendBuffer();
}

static void drawVoiceInput() {
  u8g2.clearBuffer();
  int xPos = DISPLAY_WIDTH/2;
  int yPos = DISPLAY_HEIGHT/2;
  drawIcon(IconType::Microphone, xPos - 5,  yPos);

  auto centerX = [](const char* s) -> int {
    return (128 - (int)u8g2.getStrWidth(s)) / 2;
  };
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(centerX("Was moechtest du"), yPos + 10 , "Was moechtest du");
  u8g2.drawStr(centerX("essen?"), yPos + 20, "essen?");

  u8g2.sendBuffer();
}

static void drawSendRequest() {
  u8g2.clearBuffer();
  int xPos = DISPLAY_WIDTH/2;
  int yPos = DISPLAY_HEIGHT/2;
  drawIcon(IconType::Microphone, xPos - 5,  yPos);

  auto centerX = [](const char* s) -> int {
    return (128 - (int)u8g2.getStrWidth(s)) / 2;
  };
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(centerX("Sende Anfrage"), yPos + 10 , "Sende Anfrage");
  u8g2.drawStr(centerX("an AI..."), yPos + 20, "an AI...");

  u8g2.sendBuffer();
}

static void render() {
  switch (g_screen) {
    case UIScreen::START:       drawStart(); break;
    case UIScreen::DETAIL:      drawRecipeDetail(false); break;
    case UIScreen::EDIT:        drawRecipeDetail(true);  break;
    case UIScreen::VOICE_INPUT: drawVoiceInput();        break;
    case UIScreen::VOICE_SEND:  drawSendRequest();       break;
  }
}

void ui_renderVoiceInputScreen() {
  Serial.println("[UI] render VOICE_INPUT");
  g_screen = UIScreen::VOICE_INPUT;
  render();
}
void ui_renderVoiceSendRequestScreen() {
  Serial.println("[UI] render VOICE_SEND");
  g_screen = UIScreen::VOICE_SEND;
  render();
}

void ui_init() {
  Serial.println("[UI] init()");
  u8g2.begin();
  u8g2.setPowerSave(0);
  u8g2.setContrast(255);

  g_potFiltered = 0;
  g_screen = UIScreen::START;
  g_selected = 0;
  g_detailTop = 0;
  g_detailSel = 0;
  g_statusLine = "";
  g_hasPendingConfirm = false;
  render();
}

bool ui_takeVoiceRequest() {
  if (!g_voiceRequestPending) return false;
  g_voiceRequestPending = false;
  return true;
}

void ui_showAIResult(const std::vector<UIRecipe>& recipes) {
  Serial.printf("[UI] showAIResult: recipes=%u\n", (unsigned)recipes.size());
  if (!recipes.empty())
    Serial.printf("[UI] first='%s' items=%u\n",
      recipes[0].name.c_str(), (unsigned)recipes[0].spices.size());

  std::vector<UIRecipe> top = recipes;
  g_recipes  = top;
  g_selected = clampi(g_selected, 0, (int)recipeCount() - 1);

  if (g_recipes.empty() || g_recipes[0].spices.empty()) {
    g_screen = UIScreen::START;
    g_statusLine = "Keine Gewuerze erkannt";
    render();
    return;
  }
  g_screen = UIScreen::DETAIL;
  g_detailTop = 0;
  g_detailSel = 0;
  g_statusLine = "";
  render();
}

void ui_showAIError(const char* msg) {
  Serial.printf("[UI] showAIError: %s\n", msg ? msg : "(null)");
  g_screen = UIScreen::START;
  g_statusLine = msg ? msg : "AI-Fehler";
  render();
}

void ui_setRecipes(const std::vector<UIRecipe>& recipes) {
  g_recipes = recipes;
  g_selected = clampi(g_selected, 0, (int)recipeCount() - 1);
  render();
}

void ui_setStatusLine(const char* text) {
  g_statusLine = text ? text : "";
  render();
}

void ui_goStart() {
  Serial.println("[UI] goStart -> START");
  g_screen = UIScreen::START;
  g_selected = 0;
  g_detailTop = 0;
  g_detailSel = 0;
  render();
}


void ui_tick(int potRaw) {
}

void ui_onBtnClick() {
  switch (g_screen) {
    case UIScreen::START:
      render();
      return;

    case UIScreen::VOICE_INPUT:
      g_statusLine = "";
      g_screen = UIScreen::START;
      render();
      return;

    case UIScreen::DETAIL: {
      const int total = (int)g_recipes[g_selected].spices.size();
      const int idxWeiterTop    = 0;
      const int idxWeiterBottom = total + 1;
      const int idxZurueck      = total + 2;

      if (g_detailSel == idxWeiterTop || g_detailSel == idxWeiterBottom) {
        // Bestätigen
        g_pendingRecipe       = g_recipes[g_selected];
        g_hasPendingConfirm   = true;
        ui_setStatusLine("OK");
        return;
      } else if (g_detailSel == idxZurueck) {
        // Zur Startseite zurück
        g_screen = UIScreen::START;
        render();
        return;
      } else if (g_detailSel >= 1 && g_detailSel <= total) {
        // Gewürz editieren
        g_screen = UIScreen::EDIT;
        render();
        return;
      }
      return;
    }

    case UIScreen::EDIT: {
      const int total = (int)g_recipes[g_selected].spices.size();
      const int idxWeiterTop    = 0;
      const int idxWeiterBottom = total + 1;
      const int idxZurueck      = total + 2;

      if (g_detailSel == idxWeiterTop || g_detailSel == idxWeiterBottom) {
        // Auch im EDIT: "Weiter" = bestätigen
        g_pendingRecipe       = g_recipes[g_selected];
        g_hasPendingConfirm   = true;
        ui_setStatusLine("OK");
        return;
      } else if (g_detailSel == idxZurueck) {
        // Zurück in die Detailansicht
        g_screen = UIScreen::DETAIL;
        render();
        return;
      } else {
        // Klick auf Gewürzzeile im EDIT -> zurück zur Detail (oder behavior nach Wunsch)
        g_screen = UIScreen::DETAIL;
        render();
        return;
      }
    }

    default:
      render();
      return;
  }
}



void ui_whileBtnPressed() {
  g_screen = UIScreen::VOICE_INPUT;
  g_statusLine = "AI";
  render();
}

UIState ui_getState() {
  return UIState{ g_screen, g_selected, g_detailSel };
}

bool ui_takeEditedRecipe(UIRecipe& out) {
  if (!g_hasPendingConfirm) return false;
  out = g_pendingRecipe;
  g_hasPendingConfirm = false;
  ui_setStatusLine("");
  return true;
}

void ui_nudgeSelection(int delta) {
  if (delta == 0) return;

  if (g_screen == UIScreen::DETAIL || g_screen == UIScreen::EDIT) {
    const int total = (int)g_recipes[g_selected].spices.size();
    const int Nrows = total + 3; // 0=Weiter(top), 1..total=Spices, total+1=Weiter(bottom), total+2=Zurueck
    int newSel = clampi(g_detailSel + delta, 0, Nrows - 1);

    if (newSel != g_detailSel) {
      g_detailSel = newSel;

      int desiredTop = g_detailSel - (ITEMS_PER_PAGE / 2);
      desiredTop = clampi(desiredTop, 0, max(0, Nrows - (int)ITEMS_PER_PAGE));
      if (desiredTop != g_detailTop) g_detailTop = desiredTop;

      drawRecipeDetail(g_screen == UIScreen::EDIT);
    }
  }
}


void ui_nudgeAmount(int delta) {
  if (delta == 0) return;
  if (g_screen != UIScreen::EDIT) return;

  const int total = (int)g_recipes[g_selected].spices.size();

  // Wegen "Weiter" an Index 0 liegen die Gewürze bei 1..total
  const int spiceIdx = g_detailSel - 1;
  if (spiceIdx < 0 || spiceIdx >= total) {
    // Auswahl steht auf "Weiter" (0) oder "Zurueck" (total+1) -> nichts ändern
    return;
  }

  UIRecipe& r = g_recipes[g_selected];
  UISpice&  s = r.spices[spiceIdx];

  // ±0.5 g pro Rastung, Klammer 0..9 g
  const double step = 0.5;
  const double minAmt = 0.0;
  const double maxAmt = 9.0;

  double newAmt = s.amount + delta * step;
  if (newAmt < minAmt) newAmt = minAmt;
  if (newAmt > maxAmt) newAmt = maxAmt;

  if (fabs(newAmt - s.amount) >= 0.001) {
    s.amount = newAmt;
    drawRecipeDetail(true);
  }
}

