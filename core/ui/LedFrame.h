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

  void clear() { memset(this, 0, sizeof(*this)); }
};

}  // namespace gx
