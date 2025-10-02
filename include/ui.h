#pragma once
#include <vector>
#include <string>

enum class UIScreen {
  START,
  DETAIL,
  EDIT,
  VOICE_INPUT,
  VOICE_SEND
};

struct UISpice {
  std::string name;
  double amount; // in Gramm
};

struct UIRecipe {
  std::string name;
  std::vector<UISpice> spices;
};

struct UIState {
  UIScreen screen;
  int selected;   // START: Kachelindex (0=AI, 1=Einzel), sonst Rezeptindex
  int detailSel;  // Zeilenindex im Detail/Edit
};

// Rendering / Navigation
void ui_init();
void ui_renderVoiceInputScreen();
void ui_renderVoiceSendRequestScreen();
void ui_showAIResult(const std::vector<UIRecipe>& recipes);
void ui_showAIError(const char* msg);
void ui_setRecipes(const std::vector<UIRecipe>& recipes);
void ui_setStatusLine(const char* text); // optional; kein eigener Screenwechsel
void ui_goStart();
void ui_showDetail(); // zurück in die Listenansicht (Detail)

// Interaktion
void ui_tick(int potRaw);
void ui_onBtnClick();
UIState ui_getState();
bool ui_takeEditedRecipe(UIRecipe& out);
void ui_nudgeSelection(int delta);
void ui_nudgeAmount(int delta);

// Hilfen für Rename
void ui_renameSelectedSpice(const char* newName);
bool ui_getSelectedSpiceName(char* out, size_t maxlen);
