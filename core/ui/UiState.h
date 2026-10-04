#pragma once

#include <stdint.h>

#include "engine/Project.h"
#include "ui/Controls.h"

namespace gx {

static const uint8_t kNoPad = 0xFF;
static const uint16_t kNoSlot = 0xFFFF;
// A step or piano roll pad released sooner than this was tapped; held longer, it only shows
// (or plays) what's there.
static const uint32_t kTapMaxMs = 500;  // Circuit Tracks' short-press limit too

// Tracks and patterns are shown a page at a time. B1..B8 address the tracks of the current
// track page, and pattern mode shows those tracks (columns) against a page of patterns
// (rows). In pattern mode the bottom row picks both: B1..B4 the track page and B5..B8 the
// pattern page, with no modifier.
// Note mode shows one page of steps on the top four rows; hold R3 and press B1..B8 for the rest.
static const uint8_t kStepsPerPage = 32;
static const uint8_t kNumStepPages =
    static_cast<uint8_t>((kMaxSteps + kStepsPerPage - 1) / kStepsPerPage);

static const uint8_t kTracksPerPage = kGridCols;
static const uint8_t kPatternsPerPage = kGridRows;
static const uint8_t kNumTrackPages = (kNumTracks + kTracksPerPage - 1) / kTracksPerPage;
static const uint8_t kNumPatternPages = (kNumPatterns + kPatternsPerPage - 1) / kPatternsPerPage;

// UI state shared by every mode.
struct UiState {
  uint8_t track;             // selected track
  uint8_t stepPage;          // steps shown in note mode
  uint8_t trackPage;         // tracks on B1..B8 and the pattern mode columns
  uint8_t patternPage;       // patterns on the pattern mode rows
  uint8_t projectPage;       // page shown in project mode
  uint8_t presetPage;        // page shown in preset mode, inside its window
  uint8_t presetWindow;      // which block of 8 pages preset mode is showing
  bool shiftHeld;            // Shift is held
  bool noteHeld;             // R3 is held: in note mode a pad then ends the pattern there
  bool clearHeld;            // R5 is held
  bool duplicateHeld;        // R6 is held
  bool recordHeld;           // R7 is held: in scene mode it captures instead of launching
  uint16_t duplicateSource;  // item picked as the Duplicate source (slot or step), or kNoSlot
  uint16_t faders[kNumTrackFaders];  // last reported positions, 0..kFaderMax; no function yet
  uint16_t masterFader;
  uint16_t knobs[kNumKnobs];          // mixer knobs, 0..kFaderMax; no function yet
  uint16_t mixFaders[kNumMixStrips];  // the mixer's faders
  uint16_t mixMaster;
  uint32_t nowMs;
  // How hard the pad being handled right now was hit, 1..127, or 0 from a surface that cannot
  // tell. A mode reads it while its handlePad runs and at no other time.
  uint8_t padVelocity;
  // Notes coming from a MIDI keyboard that are down right now, so the grid can show them: a
  // key you press lights the pad that plays it. More than this many at once and the oldest
  // stops being drawn, which is a drawing limit and nothing to do with what sounds.
  uint8_t playingNotes[kMaxPlayingNotes];
  uint8_t playingNoteCount;
  // Set by a mode when a pad asks for something only the controller can do, and cleared by
  // it: the Global settings pad that sends every control's CC again.
  bool sendAllRequested;  // time of the events being handled
};

// Handles a press while Duplicate is held: the first item becomes the source and later
// items are destinations. Returns true, with the source in from, for a destination.
inline bool takeDuplicateTarget(UiState& state, uint16_t item, uint16_t& from) {
  if (state.duplicateSource == kNoSlot) {
    state.duplicateSource = item;
    return false;
  }
  from = state.duplicateSource;
  return item != from;
}

}  // namespace gx
