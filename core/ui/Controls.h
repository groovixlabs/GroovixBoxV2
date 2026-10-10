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

// A page panel: a grid of pads that does nothing but show which page of each kind is open
// and switch to another, one row per kind of page and one column per page. It plays no notes
// and edits nothing, so it needs no part of the grid's layout - an APC Key 25, whose 5x8 pads
// hold the four kinds of page by the eight pages each has, with no row to spare.
static const uint8_t kNumPageKinds = 4;   // see UiController::PageKind
static const uint8_t kPagesPerKind = 8;
static const uint8_t kNumPagePads = kNumPageKinds * kPagesPerKind;
// Preset mode's pages sit inside a window of 512 voices and the model has eight of them. The
// panel gives the windows a pad row of their own, so all eight are there - a row of eight is
// the whole model, where the four buttons this used to live on were half of it.
static const uint8_t kNumPanelWindows = kPagesPerKind;
// The panel's mode buttons: the five modes that need Shift on the APC, each on a button of
// its own so the panel reaches them in one press. They light to show which one is open.
static const uint8_t kNumPanelModes = 5;
// And four arrow buttons. They scroll the piano roll, which otherwise needs Shift and a pad
// cluster; the count is RollScroll's, which is where their order comes from too.
static const uint8_t kNumPanelArrows = 4;

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
  kGroupPage,          // a page panel's pads, index pagePadIndex(row, page)
  kGroupPresetWindow,  // a page panel's window buttons, index 0..kNumPanelWindows-1
  kGroupPanelMode,     // a page panel's mode buttons, index 0..kNumPanelModes-1
  kGroupPanelArrow,    // a page panel's arrows, index 0..kNumPanelArrows-1 in RollScroll order
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

// Page panel pads are indexed by row then page, so a row is a run of eight. The row is the
// panel's own top-to-bottom order, which is the player's layout rather than the order the
// kinds happen to be declared in - UiController maps between the two.
inline uint8_t pagePadIndex(uint8_t row, uint8_t page) {
  return static_cast<uint8_t>(row * kPagesPerKind + page);
}

// Rows a panel needs: one per kind of page, and one for the preset windows.
static const uint8_t kNumPanelRows = kNumPageKinds + 1;

// What a row of a panel's pads does. The jobs, and where they sit, are settled here rather
// than in each surface, because two devices with different numbers of pad rows have to lay
// the panel out the same way or they are not the same instrument.
//
// The first kinds hang from the TOP, where they have always been, so they do not move when a
// kind is added or taken away. The LAST kind - the preset pages - sits on the BOTTOM row with
// the window row directly above it: a window holds exactly eight preset pages, so the two
// rows read downward as a row of tabs over what the open tab shows. Rows left in between are
// spare, dark, and report nothing.
enum PanelRowJob {
  kPanelRowSpare = 0,
  kPanelRowPages,
  kPanelRowWindows,
};

// Which of a device's pad rows the panel's pages and windows sit on, row 0 being the TOP one.
inline uint8_t panelPageRowAt(uint8_t padRows, uint8_t pageRow) {
  return pageRow + 1 < kNumPageKinds ? pageRow : static_cast<uint8_t>(padRows - 1);
}
inline uint8_t panelWindowRowAt(uint8_t padRows) { return static_cast<uint8_t>(padRows - 2); }

// And the inverse, for a press: what the device's row does, with pageRow set when it is a row
// of pages. Checked from the bottom up, because that is the end the pair is anchored to.
inline uint8_t panelRowJobAt(uint8_t padRows, uint8_t rowFromTop, uint8_t& pageRow) {
  if (rowFromTop + 1 == padRows) {
    pageRow = kNumPageKinds - 1;
    return kPanelRowPages;
  }
  if (rowFromTop + 2 == padRows) return kPanelRowWindows;
  if (rowFromTop + 1 < kNumPageKinds) {
    pageRow = rowFromTop;
    return kPanelRowPages;
  }
  return kPanelRowSpare;
}

}  // namespace gx
