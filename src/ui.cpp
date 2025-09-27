#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <icons.h>

#include <string>
#include <vector>

// ---------- Display pins ----------
#define OLED_CLK   19  // SCK
#define OLED_MOSI  23  // MOSI
#define OLED_CS     5  // chip select
#define OLED_DC    21  // D/C#
#define OLED_RST   14  // reset

// ---------- Potentiometer ----------
#define POT_PIN    2     // your choice
#define POT_BITS   4095  // 12-bit ADC on ESP32-C6

float potFiltered = 0.0f;
const float POT_ALPHA = 0.2f;  // 0.1 = smoother, 0.3 = snappier

static int mapPotToRange(int potRaw, int maxExclusive) {
  if (maxExclusive <= 1) return 0;
  long v = potRaw;
  long idx = (v * maxExclusive) / (POT_BITS + 1);  // 0..maxExclusive-1
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


// ---------- Select Button ----------
#define BTN_SEL    22

struct Spice {
  std::string name;
  double amount;
};

struct Recipe {
  std::string name;
  std::vector<Spice> spices;
};

// SPI constructor
U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2(
  U8G2_R0, OLED_CLK, OLED_MOSI, OLED_CS, OLED_DC, OLED_RST
);

// Virtual row index for "Back to recipes"
const int VIRTUAL_BACK = -1;


// ---------- Debounce ----------
const unsigned long DEBOUNCE_MS = 30;
struct Debounce {
  bool lastStable = true;  // HIGH idle
  bool lastRead   = true;
  unsigned long lastChangeMs = 0;
};
Debounce dbSel;


static bool pressedEdge(uint8_t pin, Debounce &db) {
  bool raw = digitalRead(pin);
  unsigned long now = millis();
  if (raw != db.lastRead) {
    db.lastRead = raw;
    db.lastChangeMs = now;
  }
  if ((now - db.lastChangeMs) > DEBOUNCE_MS && raw != db.lastStable) {
    db.lastStable = raw;
    return (raw == LOW);
  }
  return false;
}

// ---------- Data ----------
std::vector<Recipe> recipes = {
  {"Spaghetti Carbonara", {{"Black Pepper", 1}, {"Salt", 0.5}}},
  {"Chicken Tikka", {{"Cumin", 1.5}, {"Coriander", 2}, {"Turmeric", 0.5}, {"Chili Powder", 1}}},
  {"Tomato Basil Soup", {{"Basil", 2}, {"Garlic", 1}, {"Salt", 0.5}}},
  {"Pancakes", {{"Salt", 0.25}, {"Cinnamon", 0.5}}},
  {"Veggie Stir Fry", {{"Soy Sauce", 2}, {"Ginger", 1}, {"Garlic", 1}}},
  {"Beef Chili", {{"Chili Powder", 2}, {"Cumin", 1}, {"Paprika", 1}}},
  {"Garlic Shrimp", {{"Garlic", 3}, {"Paprika", 1}, {"Salt", 0.5}}},
  {"Mushroom Risotto", {{"Parsley", 1}, {"Salt", 0.5}, {"Black Pepper", 1}}},
  {"Banana Bread", {{"Cinnamon", 1}, {"Nutmeg", 0.5}, {"Salt", 0.25}}},
  {"Greek Salad", {{"Oregano", 1}, {"Salt", 0.5}, {"Black Pepper", 0.5}}}
};

static inline uint8_t recipeCount() {
  size_t n = recipes.size();
  return n > 255 ? 255 : static_cast<uint8_t>(n);
}

// ---------- Layout ----------
const uint8_t LINE_H = 10;
const uint8_t TOP_MARGIN = 12;
const uint8_t ITEMS_PER_PAGE = 4;
const uint8_t TEXT_X = 4;
const uint8_t TITLE_PAD_X = 4;

// ---------- UI state ----------
enum Screen { SCREEN_LIST, SCREEN_DETAIL, SCREEN_EDIT };
Screen screen = SCREEN_LIST;

int selected = 0;           // which recipe is highlighted on list screen
int detailTop = 0;          // first spice index shown on detail screen
int detailSel = 0;

// truncate a std::string to fit width in pixels, appending "..." if needed
static std::string truncateToWidth(const std::string& s, uint8_t maxW) {
  u8g2.setFont(FONT_TEXT);
  if (u8g2.getStrWidth(s.c_str()) <= maxW) return s;

  std::string t = s;
  while (!t.empty() && u8g2.getStrWidth((t + "...").c_str()) > maxW) {
    t.pop_back();
  }
  return t + "...";
}

static String fmtAmount(double v) {
  // show integer if close to whole number, else one decimal
  long iv = lround(v);
  if (fabs(v - iv) < 0.05) return String(iv);
  char buf[16];
  dtostrf(v, 0, 1, buf);
  return String(buf);
}

static void drawTitle(const char* title, bool showBack = false) {
  u8g2.setDrawColor(1);
  u8g2.drawHLine(0, TOP_MARGIN - 1, 128);
  

  int x = 4;
  if (showBack) {
    drawIcon(IconType::Left,x,10);
    x += 10;
  }

  u8g2.setFont(FONT_TEXT);
  u8g2.drawStr(x, 9, title);
}

// Call pattern for a screen:
static void drawList() {
  const uint8_t N = recipeCount();

  u8g2.clearBuffer();
  drawTitle("Recipes", false);

  uint8_t page = selected / ITEMS_PER_PAGE;          // internal paging only
  uint8_t start = page * ITEMS_PER_PAGE;
  uint8_t end   = min<uint8_t>(start + ITEMS_PER_PAGE, N);

  for (uint8_t i = start; i < end; ++i) {
    uint8_t line = i - start;
    uint8_t y = TOP_MARGIN + 1 + line * LINE_H + 8;
    bool isSel = (i == selected);

    if (isSel) { u8g2.drawBox(0, TOP_MARGIN + line * LINE_H, 128, LINE_H); u8g2.setDrawColor(0); }
    else        { u8g2.setDrawColor(1); }

    u8g2.setFont(FONT_TEXT);
    u8g2.drawStr(TEXT_X, y, recipes[i].name.c_str());

    if (isSel) {    
      drawIcon(IconType:: Right, 118, y);               
      u8g2.setDrawColor(1);
    }
  }

  u8g2.sendBuffer();
}


static void drawDetail() {
  const Recipe& recipe = recipes[selected];
  const int total = (int)recipe.spices.size();

  u8g2.clearBuffer();
  drawTitle(recipe.name.c_str(), true);

  if (total == 0) {
    u8g2.setFont(FONT_TEXT);
    u8g2.drawStr(TEXT_X, TOP_MARGIN + 8, "Keine Gewürze erkannt");
    u8g2.sendBuffer();
    return;
  }

  int start = detailTop;  // can be -1
  int endExclusive = min(start + (int)ITEMS_PER_PAGE, total);

  for (int i = start; i < endExclusive; ++i) {
    int line = i - start;
    uint8_t y = TOP_MARGIN + 1 + line * LINE_H + 8;
    bool isSel = (i == detailSel);

    if (isSel) { u8g2.drawBox(0, TOP_MARGIN + line * LINE_H, 128, LINE_H); u8g2.setDrawColor(0); }
    else        { u8g2.setDrawColor(1); }

    if (i == VIRTUAL_BACK) {
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Back to recipes");
      drawIcon(IconType::Right, 118, y);
    } else {
      const Spice& spice = recipe.spices[i];
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, spice.name.c_str());

      String amt = fmtAmount(spice.amount);
      u8g2_uint_t w = u8g2.getStrWidth(amt.c_str());
      u8g2.drawStr(128 - TITLE_PAD_X - w, y, amt.c_str());
    }

    if (isSel) u8g2.setDrawColor(1);
  }

  if (start > VIRTUAL_BACK)         drawIcon(IconType::ChevUp,   122, TOP_MARGIN + 8);
  if (endExclusive < total)         drawIcon(IconType::ChevDown, 122, 64 - 2);

  u8g2.sendBuffer();
}


static void drawEdit() {
  const Recipe& recipe = recipes[selected];
  const int total = (int)recipe.spices.size();

  u8g2.clearBuffer();
  drawTitle(recipe.name.c_str(), true);

  if (total == 0) {
    u8g2.setFont(FONT_TEXT);
    u8g2.drawStr(TEXT_X, TOP_MARGIN + 8, "Keine Gewürze erkannt");
    u8g2.sendBuffer();
    return;
  }

  int start = detailTop;  // can be -1
  int endExclusive = min(start + (int)ITEMS_PER_PAGE, total);

  for (int i = start; i < endExclusive; ++i) {
    int line = i - start;
    uint8_t y = TOP_MARGIN + 1 + line * LINE_H + 8;
    bool isSel = (i == detailSel);

    if (isSel) { u8g2.drawBox(0, TOP_MARGIN + line * LINE_H, 128, LINE_H); u8g2.setDrawColor(0); }
    else        { u8g2.setDrawColor(1); }

    if (i == VIRTUAL_BACK) {
      u8g2.setFont(FONT_TEXT);
      u8g2.drawStr(TEXT_X, y, "Back to recipes");
      drawIcon(IconType::Right, 118, y);
    } else {
      const Spice& spice = recipe.spices[i];
      u8g2.drawStr(TEXT_X, y, spice.name.c_str());

      String amount = fmtAmount(spice.amount);
      u8g2_uint_t w = u8g2.getStrWidth(amount.c_str());
      if (isSel) {
        u8g2.drawStr(128 - TITLE_PAD_X - w, y, amount.c_str());
      }
    }

    if (isSel) u8g2.setDrawColor(1);
  }

  if (start > VIRTUAL_BACK)         drawIcon(IconType::ChevUp,   122, TOP_MARGIN + 8);
  if (endExclusive < total)         drawIcon(IconType::ChevDown, 122, 64 - 2);

  u8g2.sendBuffer();
}

// ---------- Setup/Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  potFiltered = analogRead(POT_PIN);
  pinMode(BTN_SEL,  INPUT_PULLUP);
  u8g2.begin();
  drawList();
}

void loop() {
  // read and filter pot
  int potRaw = analogRead(POT_PIN);
  Serial.println(potRaw);
  potFiltered = POT_ALPHA * potRaw + (1.0f - POT_ALPHA) * potFiltered;
  int potVal = (int)potFiltered;

  if (screen == SCREEN_LIST) {
  const int totalRecipes = recipeCount();
  int newSel = mapPotToRange(potVal, totalRecipes); // 0..N-1
  if (newSel != selected) {
    selected = newSel;
    drawList();
  }

  if (pressedEdge(BTN_SEL, dbSel)) {
    detailTop = 0;
    detailSel = 0;   // start at first spice, not the Back row
    screen = SCREEN_DETAIL;
    drawDetail();
  }

 } else if (screen == SCREEN_DETAIL) {
  const int total = recipes[selected].spices.size();
  if (total > 0) {
    // Map pot to [-1..total-1], where -1 is the Back row
    int newSel = mapPotToRange(potVal, total + 1) - 1;
    if (newSel != detailSel) {
      detailSel = newSel;

      int desiredTop = detailSel - (ITEMS_PER_PAGE / 2);
      int maxTop = max(VIRTUAL_BACK, total - (int)ITEMS_PER_PAGE);
      desiredTop = max(VIRTUAL_BACK, min(desiredTop, maxTop));
      if (desiredTop != detailTop) detailTop = desiredTop;

      drawDetail();
    }
  }

  if (pressedEdge(BTN_SEL, dbSel) && total > 0) {
    if (detailSel == VIRTUAL_BACK) {
      screen = SCREEN_LIST;   // Back row selected
      drawList();
    } else {
      screen = SCREEN_EDIT;   // Edit spice
      drawEdit();
    }
  
} } else if (screen == SCREEN_EDIT) {
  const int total = recipes[selected].spices.size();

  if (detailSel != VIRTUAL_BACK) {
    // adjust amount with pot (0.5 steps, max 9)
    Recipe& recipe = recipes[selected];
    Spice&  spice  = recipe.spices[detailSel];

    const double maxAmt = 9.0;
    double raw = (potVal * maxAmt) / (double)POT_BITS;
    double newAmt = quantizeStepClamped(raw, 0.5, maxAmt);

    if (fabs(newAmt - spice.amount) >= 0.05) {
      spice.amount = newAmt;
      drawEdit();
    }
  }

  if (pressedEdge(BTN_SEL, dbSel)) {
    if (detailSel == VIRTUAL_BACK) {
      screen = SCREEN_LIST;   // Back row exits to recipes
      drawList();
    } else {
      screen = SCREEN_DETAIL; // save and return
      drawDetail();
    }
  }
}
  delay(5);
}
