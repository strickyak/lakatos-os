#pragma once

#include <stdint.h>

enum LakatosGraffitiPane : uint8_t {
  LAKATOS_PANE_LETTERS = 1,
  LAKATOS_PANE_DIGITS  = 2,
};

// Special pseudo-character codes returned by lakatosRecognizeGraffitiStroke:
constexpr char LAKATOS_GRAFFITI_NONE      = 0;
constexpr char LAKATOS_GRAFFITI_BACKSPACE = '\b';
constexpr char LAKATOS_GRAFFITI_SHIFT     = '\x0E';  // Cycle shift: abc -> Abc -> ABC -> abc

struct LakatosGraffitiPt {
  int16_t x;
  int16_t y;
};

struct LakatosGraffitiResult {
  char    ch;          // Recognized ASCII char or LAKATOS_GRAFFITI_* control code
  uint8_t confidence;  // 0..100 similarity score
};

// Recognizes a single stylus stroke drawn in `pane` (LAKATOS_PANE_LETTERS or LAKATOS_PANE_DIGITS).
LakatosGraffitiResult lakatosRecognizeGraffitiStroke(const LakatosGraffitiPt* pts,
                                                     int count,
                                                     uint8_t pane);

// Opens a modal 2-page visual cheat-sheet showing the Palm Graffiti single-stroke
// alphabet (Page 1: Letters A-Z & gestures; Page 2: Digits 0-9 & math symbols)
// with green start dots.
void lakatosRunGraffitiHelpModal();
