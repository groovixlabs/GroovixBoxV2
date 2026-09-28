#pragma once

#include <stdint.h>

#include "ui/Rgb.h"

namespace gx {

// Slot colours used by the project, pattern and preset modes.
static const Rgb kSelectedColor = {0, 255, 0};      // green: the slot in use
static const Rgb kFilledColor = {0, 45, 150};       // dim blue: holds data
static const Rgb kEmptyColor = {24, 24, 24};        // grey: empty
static const Rgb kCopySourceColor = {255, 255, 255};  // white: picked Duplicate source

// Colour of a track (B1..B8).
Rgb trackColor(uint8_t track);

// The step the playhead is on: white while playing, red while recording, so the grid says at
// a glance that what you play is being written and where it will land. Dimmer on a step with
// nothing on it, since there the pad is only the playhead.
static const uint8_t kPlayheadLevel = 90;
inline Rgb playheadColor(bool recording, bool onActiveStep) {
  const Rgb color = recording ? Rgb{255, 0, 0} : Rgb{255, 255, 255};
  return onActiveStep ? color : dim(color, kPlayheadLevel);
}

// Anything waiting to happen blinks at 2 Hz: the page the playhead is on, a scene queued for
// the next bar. One phase for the whole surface, so they blink together.
static const uint32_t kBlinkHalfPeriodMs = 250;
inline bool blinkOn(uint32_t nowMs) { return (nowMs / kBlinkHalfPeriodMs) % 2 == 0; }

}  // namespace gx
