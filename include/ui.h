#pragma once
#include <vector>
#include <string>

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 64

struct UISpice { std::string name; double amount; };
struct UIRecipe { std::string name; std::vector<UISpice> spices; };

enum class UIScreen { START, DETAIL, EDIT, VOICE_INPUT, VOICE_SEND };
struct UIState { UIScreen screen; int selected; int detailSel; };

void ui_init();
void ui_setRecipes(const std::vector<UIRecipe>& recipes);
void ui_tick(int potRaw);
void ui_onBtnClick();
void ui_whileBtnPressed();
UIState ui_getState();
bool ui_takeEditedRecipe(UIRecipe& out);
void ui_setStatusLine(const char* text);

void ui_renderVoiceInputScreen();
void ui_renderVoiceSendRequestScreen();
bool ui_takeVoiceRequest();

void ui_showAIResult(const std::vector<UIRecipe>& recipes);
void ui_showAIError(const char* msg);

void ui_goStart();

void ui_nudgeSelection(int delta);
void ui_nudgeAmount(int delta);

void ui_renameSelectedSpice(const char* newName);

void ui_showDetail();

bool ui_getSelectedSpiceName(char* out, size_t maxlen);

void ui_startBlinkSelected();
void ui_stopBlinkSelected();



