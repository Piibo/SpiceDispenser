#include "ui.h"
#include "pins.h"
#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include "icons.h"
#include <cmath>

// ---- interner UI-Status ----
static std::vector<UIRecipe> g_recipes;
static UIScreen g_screen = UIScreen::START;
static int g_selected = 0;    // Rezeptindex (oder Kachelindex im START)
static int g_detailTop = 0;   // Scroll-Offset in Detail/Edit
static int g_detailSel = 0;   // Cursor-Zeile in Detail/Edit
static String g_statusLine;   // optional; wird nicht eigens gezeichnet
static bool g_hasPendingConfirm = false;
static UIRecipe g_pendingRecipe;
static int g_startSel = 0;    // 0=AI-Gericht, 1=Einzel-Gewuerz

// Layout-Konstanten
static const uint8_t LINE_H = 10;
static const uint8_t TOP_MARGIN = 12;
static const uint8_t ITEMS_PER_PAGE = 4;
static const uint8_t TEXT_X = 4;
static const uint8_t TITLE_PAD_X = 4;

U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(
  U8G2_R0, OLED_CLK, OLED_MOSI, OLED_CS, OLED_DC, OLED_RST
);

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t recipeCount() {
  size_t n = g_recipes.size();
  return n > 255 ? 255 : static_cast<uint8_t>(n);
}

// --------- Title / Header ----------
static void drawTitle(const char* title, bool /*showBack*/) {
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(TITLE_PAD_X, 10, title);
  u8g2.drawHLine(0, TOP_MARGIN - 1, 128);
  u8g2.setFont(FONT_TEXT);
}

// --------- START (Kacheln) ----------
static void drawStart() {
  u8g2.clearBuffer();
  drawTitle("Lass uns kochen!", false);

  const uint8_t TILE_W   = 56;
  const uint8_t TILE_H   = 36;
  const uint8_t TILE_RAD = 4;
  const uint8_t TILE_GAP = 8;

  const int totalW = (2 * TILE_W) + TILE_GAP;
  const int x0 = (128 - totalW) / 2;
  const int x1 = x0 + TILE_W + TILE_GAP;
  const int y  = TOP_MARGIN + 4;

  struct Tile { const char* l1; const char* l2; };
  const Tile tiles[2] = {
    { "AI-",     "Gericht" },
    { "Einzel-", "Gewuerz" }
  };

  auto drawTile = [&](int tx, int ty, bool selected, const Tile& t) {
    if (selected) {
      u8g2.setDrawColor(1);
      u8g2.drawRBox(tx, ty, TILE_W, TILE_H, TILE_RAD);
      u8g2.setDrawColor(0);
    } else {
      u8g2.setDrawColor(1);
      u8g2.drawRFrame(tx, ty, TILE_W, TILE_H, TILE_RAD);
    }

    u8g2.setFont(FONT_TEXT);
    auto cx = [&](const char* s){ return tx + (TILE_W - (int)u8g2.getStrWidth(s)) / 2; };
    int textY1 = ty + (TILE_H/2) - 3;
    int textY2 = textY1 + 10;

    u8g2.drawStr(cx(t.l1), textY1, t.l1);
    u8g2.drawStr(cx(t.l2), textY2, t.l2);
    if (selected) u8g2.setDrawColor(1);
  };

  drawTile(x0, y, (g_startSel == 0), tiles[0]);
  drawTile(x1, y, (g_startSel == 1), tiles[1]);

  u8g2.sendBuffer();
}

// --------- DETAIL/EDIT-Liste ----------
static String fmtAmount(double v) {
  long iv = lround(v);
  if (fabs(v - iv) < 0.05) return String(iv);
  char buf[16];
  dtostrf(v, 0, 1, buf);
  return String(buf);
}

static void drawRecipeDetail(bool editing) {
  (void)editing;
  const UIRecipe& recipe = g_recipes[g_selected];
  const int total = (int)recipe.spices.size();
  const int Nrows = total + 3; // Weiter(oben) + Spices + Weiter(unten) + Zurueck

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

    if (isSel) {
      u8g2.drawBox(0, TOP_MARGIN + line * LINE_H, 128, LINE_H);
      u8g2.setDrawColor(0);
    } else {
      u8g2.setDrawColor(1);
    }

    if (i == 0 || i == total + 1) {
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Weiter");
      drawIcon(IconType::Check, 118, y);
    } else if (i == total + 2) {
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Zurueck");
      drawIcon(IconType::Left, 118, y);
    } else {
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

// --------- Voice Screens ----------
static void drawVoiceInput() {
  u8g2.clearBuffer();
  int xPos = 128/2;
  int yPos = 64/2;
  drawIcon(IconType::Microphone, xPos - 5,  yPos);

  auto centerX = [](const char* s) -> int {
    return (128 - (int)u8g2.getStrWidth(s)) / 2;
  };
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(centerX("Was moechtest du"), yPos + 10 , "Was moechtest du");
  u8g2.drawStr(centerX("essen?"),           yPos + 20 , "essen?");
  u8g2.sendBuffer();
}

static void drawSendRequest() {
  u8g2.clearBuffer();
  int xPos = 128/2;
  int yPos = 64/2;
  drawIcon(IconType::Microphone, xPos - 5,  yPos);

  auto centerX = [](const char* s) -> int {
    return (128 - (int)u8g2.getStrWidth(s)) / 2;
  };
  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(centerX("Sende Anfrage"), yPos + 10 , "Sende Anfrage");
  u8g2.drawStr(centerX("an AI..."),      yPos + 20 , "an AI...");
  u8g2.sendBuffer();
}

// --------- Top-Level Render ----------
static void render() {
  switch (g_screen) {
    case UIScreen::START:       drawStart();        break;
    case UIScreen::DETAIL:      drawRecipeDetail(false); break;
    case UIScreen::EDIT:        drawRecipeDetail(true);  break;
    case UIScreen::VOICE_INPUT: drawVoiceInput();   break;
    case UIScreen::VOICE_SEND:  drawSendRequest();  break;
  }
}

// --------- Public API ----------
void ui_renderVoiceInputScreen() {
  g_screen = UIScreen::VOICE_INPUT;
  render();
}
void ui_renderVoiceSendRequestScreen() {
  g_screen = UIScreen::VOICE_SEND;
  render();
}

void ui_init() {
  u8g2.begin();
  u8g2.setPowerSave(0);
  u8g2.setContrast(255);

  g_screen = UIScreen::START;
  g_selected = 0;
  g_detailTop = 0;
  g_detailSel = 0;
  g_statusLine = "";
  g_hasPendingConfirm = false;
  render();
}

void ui_showAIResult(const std::vector<UIRecipe>& recipes) {
  g_recipes = recipes;
  g_selected = 0;

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
  g_screen = UIScreen::START;
  g_startSel = 0;
  g_detailTop = 0;
  g_detailSel = 0;
  render();
}

void ui_showDetail() {
  if (g_recipes.empty()) { g_screen = UIScreen::START; render(); return; }
  g_screen = UIScreen::DETAIL;
  drawRecipeDetail(false);
}

void ui_tick(int /*potRaw*/) {
  // kein Blink, keine extra Animation
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
        g_pendingRecipe     = g_recipes[g_selected];
        g_hasPendingConfirm = true;
        ui_setStatusLine("OK");
        return;
      } else if (g_detailSel == idxZurueck) {
        g_screen = UIScreen::START;
        render();
        return;
      } else if (g_detailSel >= 1 && g_detailSel <= total) {
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
        g_pendingRecipe     = g_recipes[g_selected];
        g_hasPendingConfirm = true;
        ui_setStatusLine("OK");
        return;
      } else if (g_detailSel == idxZurueck) {
        g_screen = UIScreen::DETAIL;
        render();
        return;
      } else {
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

UIState ui_getState() {
  int sel = (g_screen == UIScreen::START) ? g_startSel : g_selected;
  return UIState{ g_screen, sel, g_detailSel };
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

  if (g_screen == UIScreen::START) {
    int newSel = clampi(g_startSel + delta, 0, 1);
    if (newSel != g_startSel) { g_startSel = newSel; drawStart(); }
    return;
  }

  if (g_screen == UIScreen::DETAIL || g_screen == UIScreen::EDIT) {
    const int total = (int)g_recipes[g_selected].spices.size();
    const int Nrows = total + 3;
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
  const int spiceIdx = g_detailSel - 1; // 1..total

  if (spiceIdx < 0 || spiceIdx >= total) return;

  UIRecipe& r = g_recipes[g_selected];
  UISpice&  s = r.spices[spiceIdx];

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

void ui_renameSelectedSpice(const char* newName) {
  if (!newName || !*newName) return;
  if (g_recipes.empty()) return;
  if (g_selected < 0 || g_selected >= (int)g_recipes.size()) return;

  const int total = (int)g_recipes[g_selected].spices.size();
  const int spiceIdx = g_detailSel - 1; // 1..total
  if (spiceIdx < 0 || spiceIdx >= total) return;

  g_recipes[g_selected].spices[spiceIdx].name = std::string(newName);

  if (g_screen == UIScreen::DETAIL || g_screen == UIScreen::EDIT) {
    drawRecipeDetail(g_screen == UIScreen::EDIT);
  }
}

bool ui_getSelectedSpiceName(char* out, size_t maxlen) {
  if (!out || maxlen == 0) return false;
  if (g_screen != UIScreen::DETAIL && g_screen != UIScreen::EDIT) return false;

  const int total = (int)g_recipes[g_selected].spices.size();
  const int spiceIdx = g_detailSel - 1;
  if (spiceIdx < 0 || spiceIdx >= total) return false;

  const std::string& nm = g_recipes[g_selected].spices[spiceIdx].name;
  strncpy(out, nm.c_str(), maxlen - 1);
  out[maxlen - 1] = '\0';
  return true;
}
