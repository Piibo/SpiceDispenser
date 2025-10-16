#pragma once
#include <Arduino.h>

/*
  -----------------------------------------------------------------------------
  SpiceDispenser – Spice Lexicon (Whitelist)
  -----------------------------------------------------------------------------
  Contains all allowed spice names and mixtures for AI matching and validation.
  All entries must be lowercase. The matching logic supports normalization
  (e.g., replaces ä/ö/ü/ß with ae/oe/ue/ss).
  -----------------------------------------------------------------------------
*/

inline constexpr const char* SPICE_LEXICON[] = {
  // Basics
  "salz", "meersalz", "pfeffer", "schwarzer pfeffer",
  "weisser pfeffer", "weißer pfeffer", "gruner pfeffer", "grüner pfeffer",

  // Chili / Paprika
  "chili", "chiliflocken", "chilipulver", "paprika", "paprikapulver",
  "paprika edelsuss", "paprika edelsüß", "paprika rosenscharf",
  "smoked paprika", "geraeucherte paprika", "geräucherte paprika",

  // Kräuter klassisch
  "basilikum", "oregano", "thymian", "rosmarin", "majoran", "salbei",
  "dill", "schnittlauch", "petersilie", "estragon", "bohnenkraut", "lorbeer",

  // Samen & Früchte
  "koriander", "koriandersaat", "kreuzkuemmel", "kreuzkümmel", "cumin",
  "fenchelsamen", "anis", "senfsamen", "schwarzkümmel", "schwarzkuemmel",
  "kuemmel", "kümmel", "mohn",

  // Wurzeln & Rinden
  "ingwer", "kurkuma", "zimt", "muskat", "muskatnuss", "galgant",

  // Zwiebel / Knoblauch getrocknet
  "knoblauchgranulat", "knoblauchpulver", "zwiebelgranulat", "zwiebelpulver",

  // Beeren / Nelken / Piment / Kardamom
  "piment", "nelken", "kardamom", "wacholder",

  // Pfeffermischungen
  "bunter pfeffer", "pfeffermischung",

  // Mischungen international
  "curry", "currypulver", "garam masala", "tandoori masala", "baharat",
  "ras el hanout", "zaatar", "za'atar", "dukkah", "advieh", "berbere",
  "chinesisches funf gewurze", "chinesisches fünf gewürze", "five spice",
  "fuenf gewuerze", "fünf gewürze",

  // Kräutermischungen
  "kraeuter der provence", "kräuter der provence", "italienische kraeuter",
  "italienische kräuter", "pizza gewuerz", "pizza gewürz",
  "bruschetta gewuerz", "bruschetta gewürz",

  // Rauch & Spezial
  "rauchsalz", "selleriesalz", "zitronenpfeffer", "knoblauchpfeffer"
};

inline constexpr size_t SPICE_LEXICON_COUNT =
    sizeof(SPICE_LEXICON) / sizeof(SPICE_LEXICON[0]);
