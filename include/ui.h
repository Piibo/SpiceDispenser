#pragma once
#include <vector>
#include <string>

/*
  -----------------------------------------------------------------------------
  UI Module – Public Interface
  -----------------------------------------------------------------------------
  Screens:
    START       - entry screen (tiles: AI, Single Dispense, etc.)
    DETAIL      - recipe list / detail view
    EDIT        - edit amounts/names
    VOICE_INPUT - voice capture UI
    VOICE_SEND  - sending/request-in-flight UI
  -----------------------------------------------------------------------------
*/

enum class UIScreen {
  START,
  DETAIL,
  EDIT,
  VOICE_INPUT,
  VOICE_SEND
};

struct UISpice {
  std::string name;
  double amount; // grams
};

struct UIRecipe {
  std::string name;
  std::vector<UISpice> spices;
};

// Transient UI state (query-only for other modules)
struct UIState {
  UIScreen screen;
  int selected;   // START: tile index (0=AI, 1=Single), otherwise recipe index
  int detailSel;  // row index in DETAIL/EDIT
};

/* ===========================
 * Rendering / Navigation
 * =========================== */
void ui_init();
void ui_renderVoiceInputScreen();
void ui_renderVoiceSendRequestScreen();
void ui_showAIResult(const std::vector<UIRecipe>& recipes);
void ui_showAIError(const char* msg);
void ui_setRecipes(const std::vector<UIRecipe>& recipes);
void ui_setStatusLine(const char* text);   // optional; does not change screen
void ui_goStart();
void ui_showDetail();

/* ===========================
 * Interaction
 * =========================== */
void ui_tick(int potRaw);
void ui_onBtnClick();
UIState ui_getState();
bool ui_takeEditedRecipe(UIRecipe& out);   // returns true if a recipe was edited
void ui_nudgeSelection(int delta);
void ui_nudgeAmount(int delta);

/* ===========================
 * Rename Helpers
 * =========================== */
void ui_renameSelectedSpice(const char* newName);
bool ui_getSelectedSpiceName(char* out, size_t maxlen);

/* ===========================
 * Hints / UX toggles
 * =========================== */
void ui_setManualHintEnabled(bool on);
