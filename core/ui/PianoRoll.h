#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/LedFrame.h"
#include "ui/UiState.h"

namespace gx {

// What Shift + B5..B8 do in the piano roll.
enum RollScroll {
  kRollUp = 0,  // B5: higher notes
  kRollDown,    // B6: lower notes
  kRollLeft,    // B7: earlier steps
  kRollRight,   // B8: later steps
  kNumRollScrolls,
};

// Note mode's piano roll, for a track that turns it on in scale mode. Columns are 8 steps and
// rows are 8 notes of the track's keyboard scale, lowest at the bottom, so the whole grid
// edits one stretch of the pattern:
//  - tap a pad to add that note to the step's chord, tap it again to take it out; holding a
//    pad plays its note without changing the step
//  - Clear + pad clears the step, Duplicate + pad + pad copies it
//  - Shift + a pad on the bottom row makes its step the pattern's last
//  - Shift + B5/B6 scroll the notes up/down and Shift + B7/B8 the steps left/right, 4 at a
//    time so half the view stays in sight; hold R3 + B1..B8 to jump to a step page
// Each track keeps its own view.
class PianoRoll {
 public:
  explicit PianoRoll(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed);
  void render(const UiState& state, LedFrame& frame) const;
  void scroll(UiState& state, uint8_t direction);  // RollScroll
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
