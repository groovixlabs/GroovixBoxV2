#pragma once

#include <stdint.h>

namespace gx {

// Physical layout of the control surface.
static const uint8_t kGridCols = 8;
static const uint8_t kGridRows = 8;
static const uint8_t kNumPads = kGridCols * kGridRows;
static const uint8_t kNumRightButtons = 8;   // R1..R8, one beside each pad row
static const uint8_t kNumBottomButtons = 8;  // B1..B8, one below each pad column
static const uint8_t kNumTrackFaders = 8;    // one below each track button
// Plus a Shift button in the bottom-right corner, below R8 and beside B8, and a master
// fader below Shift.
static const uint16_t kFaderMax = 1023;  // fader positions run from 0 (bottom) to here (top)

// The mixer, to the right of the grid, laid out like an Akai MIDI Mix: 8 strips, each with
// three knobs above buttons A1 and A2 and a fader, and a column on the right with a button
// beside each knob row, one beside the A1 row, and the mixer's master fader below them.
static const uint8_t kNumMixStrips = 8;
static const uint8_t kKnobRows = 3;
static const uint8_t kNumKnobs = kNumMixStrips * kKnobRows;
static const uint8_t kNumMixButtonRows = 2;  // A1 and A2
static const uint8_t kNumMixButtons = kNumMixStrips * kNumMixButtonRows;
static const uint8_t kNumMixSideButtons = kKnobRows + 1;
// The last side button, beside the A1 row, is the mixer's Shift: hold it and press a mute
// button to solo that track instead.
static const uint8_t kMixShiftButton = kNumMixSideButtons - 1;

enum ControlGroup {
  kGroupPad = 0,
  kGroupRight,
  kGroupBottom,
  kGroupShift,        // index is always 0
  kGroupFader,        // index 0..kNumTrackFaders-1
  kGroupMasterFader,  // index is always 0
  kGroupKnob,         // mixer knobs, index 0..kNumKnobs-1
  kGroupMixButton,    // mixer A1 row 0..kNumMixStrips-1, then the A2 row
  kGroupMixSide,      // the right-hand column: knob rows 1..3, then the A1 row
  kGroupMixFader,     // index 0..kNumMixStrips-1
  kGroupMixMaster,    // index is always 0
};

// A press or release of a pad or button, or a fader's new position. Pads are indexed
// row * kGridCols + col, with row 0 at the top and col 0 at the left. Buttons and faders
// are indexed from 0 (R1 / B1 / the fader below B1).
struct ControlEvent {
  uint8_t group;   // ControlGroup
  uint8_t index;
  bool pressed;    // pads and buttons
  uint16_t value;  // faders: position, 0..kFaderMax
  // How hard a pad was hit, 1..127, or 0 from a surface that cannot tell - a mouse, or a pad
  // that sends the same number every time. Zero means "no opinion", not silence, so what is
  // recorded keeps the step's velocity instead.
  // True for the positions a surface reports once at start-up, like a hardware scan: they say
  // where a control already is, so they are recorded without sending its MIDI CC.
  bool initial;
  uint8_t velocity;
};

// Keyboard notes drawn as down at once. A hand covers fewer than this; it bounds the state
// the UI keeps, not what the sequencer will play.
static const uint8_t kMaxPlayingNotes = 8;

inline uint8_t padIndex(uint8_t row, uint8_t col) { return row * kGridCols + col; }

// Mixer knobs and buttons are indexed row by row from the top left, so knob (row, strip) is
// knobIndex(row, strip) and the A2 button of strip 0 is mixButtonIndex(1, 0).
inline uint8_t knobIndex(uint8_t row, uint8_t strip) {
  return static_cast<uint8_t>(row * kNumMixStrips + strip);
}
inline uint8_t mixButtonIndex(uint8_t row, uint8_t strip) {
  return static_cast<uint8_t>(row * kNumMixStrips + strip);
}

}  // namespace gx
