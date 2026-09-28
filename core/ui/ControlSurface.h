#pragma once

#include "ui/Controls.h"
#include "ui/LedFrame.h"

namespace gx {

// The pads, buttons, faders and LEDs: an SDL window, the Teensy's key matrix, ADC and
// LED drivers, an iPad touch view...
class ControlSurface {
 public:
  // Fetches the next press, release or fader move. Returns false when none are pending.
  // Faders report their position when it changes, and every fader's position once at start.
  virtual bool pollEvent(ControlEvent& event) = 0;
  // Displays a complete set of LED colours.
  virtual void show(const LedFrame& frame) = 0;

 protected:
  ~ControlSurface() {}  // see EventSink
};

}  // namespace gx
