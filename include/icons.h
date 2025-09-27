#pragma once
#include <U8g2lib.h>

#define FONT_TEXT        u8g2_font_6x10_tf
#define FONT_ICON_ARROW  u8g2_font_open_iconic_arrow_1x_t
#define FONT_ICON_ALL    u8g2_font_open_iconic_all_1x_t

extern U8G2_SSD1309_128X64_NONAME0_F_4W_SW_SPI u8g2;

enum class IconType { Left, Right, Up, Down, ChevLeft, ChevRight, ChevUp, ChevDown };

static void drawIcon(IconType t, int x, int y) {
  switch (t) {
    case IconType::Left:   u8g2.setFont(FONT_ICON_ARROW); u8g2.drawGlyph(x,y,0x0041); break;
    case IconType::Right:  u8g2.setFont(FONT_ICON_ARROW); u8g2.drawGlyph(x,y,0x0042); break;
    //case IconType::Up:     u8g2.setFont(FONT_ICON_ARROW); u8g2.drawGlyph(x,y,0x0043); break;
    //case IconType::Down:   u8g2.setFont(FONT_ICON_ARROW); u8g2.drawGlyph(x,y,0x0044); break;

    case IconType::ChevLeft:  u8g2.setFont(FONT_ICON_ALL); u8g2.drawGlyph(x,y,0x0051); break;
    case IconType::ChevRight: u8g2.setFont(FONT_ICON_ALL); u8g2.drawGlyph(x,y,0x0052); break;
    //case IconType::ChevUp:    u8g2.setFont(FONT_ICON_ALL); u8g2.drawGlyph(x,y,0x0053); break;
    //case IconType::ChevDown:  u8g2.setFont(FONT_ICON_ALL); u8g2.drawGlyph(x,y,0x0050); break;
  }
  // switch back when you draw text
  u8g2.setFont(FONT_TEXT);
}
