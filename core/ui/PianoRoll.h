#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/LedFrame.h"
#include "ui/UiState.h"

namespace gx {

// Where Shift + the roll's arrow cluster moves the view.
enum RollScroll {
  kRollUp = 0,  // higher notes
  kRollDown,    // lower notes
  kRollLeft,    // earlier steps
  kRollRight,   // later steps
  kNumRollScrolls,
};

// How far a scroll moves the view: one step, or one note of the track's scale. Every arrow
// moves by this, the roll's own pad cluster and a page panel's arrow buttons alike - the
// amount belongs to the roll, not to whichever arrow was pressed, so the two cannot drift
// apart and there is nothing to remember about which one you reached for.
static const uint8_t kRollScrollBy = 1;

// Note mode's piano roll, for a track that turns it on in scale mode. Columns are 8 steps and
// rows are 8 notes of the track's keyboard scale, lowest at the bottom, so the whole grid
// edits one stretch of the pattern:
//  - tap a pad to add that note to the step's chord, tap it again to take it out; holding a
//    pad plays its note without changing the step
//  - Clear + pad clears the step, Duplicate + pad + pad copies it
//  - R1 + a pad makes its step the pattern's last
//  - Shift + an arrow cluster at the bottom right scrolls the view: left (8,6), down (8,7),
//    right (8,8) and up (7,7), one step or one scale note a press.
//    Hold R1 + B1..B8 to jump to a step page, which is how a distant step is reached
// Each track keeps its own view.
class PianoRoll {
 public:
  explicit PianoRoll(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed);
  void render(const UiState& state, LedFrame& frame) const;
  // RollScroll, by kRollScrollBy steps or scale degrees.
  void scroll(UiState& state, uint8_t direction);
  bool canScroll(const UiState& state, uint8_t direction) const;
  // Shows a step page from its first step.
  void showStepPage(UiState& state, uint8_t page);
  // Stops notes still sounding from held pads.
  void reset();

 private:
  uint16_t firstStep(const UiState& state) const;
  int bottomDegree(uint8_t track) const;
  uint8_t noteForDegree(uint8_t track, int degree) const;
  void setFirstStep(UiState& state, int first);
  // Adds the note to the step's chord, or takes it out.
  void toggleNote(uint8_t track, uint16_t step, uint8_t note);

  Sequencer& sequencer_;
  uint16_t firstSteps_[kNumTracks];  // first step shown, or kNoSlot to follow the step page
  int16_t noteOffsets_[kNumTracks];  // scale degrees scrolled from the track's octave root
  uint8_t heldNotes_[kNumPads];      // the note each held pad is sounding, or kInvalidNote
  uint16_t heldSteps_[kNumPads];     // the step each held pad is on
  uint32_t heldSince_[kNumPads];     // when each pad was pressed
  uint8_t heldTrack_;
};

}  // namespace gx
