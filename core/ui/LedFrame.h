#pragma once

#include <string.h>

#include "ui/Controls.h"
#include "ui/Rgb.h"

namespace gx {

// Colour of every LED on the surface.
struct LedFrame {
  Rgb pads[kNumPads];
  Rgb right[kNumRightButtons];
  Rgb bottom[kNumBottomButtons];
  Rgb shift;
  Rgb mixButtons[kNumMixButtons];    // the mixer's A1 and A2 rows
  Rgb mixSide[kNumMixSideButtons];   // the mixer's right-hand column
  Rgb pages[kNumPagePads];           // a page panel, a row of pages per kind
  Rgb pageWindows[kNumPanelWindows]; // and its window row, above the preset pages
  Rgb panelModes[kNumPanelModes];    // and its mode buttons, lit for the mode that is open
  Rgb panelArrows[kNumPanelArrows];  // and its arrows, lit where the view can still move

  void clear() { memset(this, 0, sizeof(*this)); }
};

}  // namespace gx
