#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Mode.h"
#include "ui/PianoRoll.h"

namespace gx {

static const uint8_t kNumStepPads = kStepsPerPage;  // top four rows: one page of steps

// R3: the top four rows are the selected track's steps, the bottom four a keyboard of the
// project's scale (see ScaleMode). It starts on the scale root at bottom-left and rises one
// scale note per pad, left to right and row by row upwards; every row starts on the root
// unless the scale has 8 or more notes per octave.
//  - tap an empty step to switch it on with the track's last note; pressing a lit step shows
//    its notes and never switches it off: only Clear + step removes it (Duplicate copies it)
//  - Shift + a step makes it the pattern's last step, which then lights red
//  - hold R3 and press B1..B8 to show another page of 32 steps
//  - hold note keys and press a step to give it that chord
//  - hold steps and press note keys to add those notes, or take out ones already there
//  - note pads play their note, and while playing + recording write it at the playhead
//  - Shift + the leftmost/rightmost key moves the keyboard down/up an octave, per track
// A track can play a chord from one key (scale mode, bottom row TRIAD or 7TH): every key then
// gives its scale degree with two or three more notes stacked on top, and Shift + a key plays
// that degree alone.
// A track can show a piano roll instead (scale mode, bottom row pad 6); see PianoRoll.
class NoteMode : public Mode {
 public:
  explicit NoteMode(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  void reset() override;

  // The piano roll of tracks that show one.
  void scrollRoll(UiState& state, uint8_t direction) { roll_.scroll(state, direction); }
  bool canScrollRoll(const UiState& state, uint8_t direction) const {
    return roll_.canScroll(state, direction);
  }
  void showRollStepPage(UiState& state, uint8_t page) { roll_.showStepPage(state, page); }

 private:
  // Note of a keyboard pad in the track's octave and the current scale, or kInvalidNote
  // above MIDI note 127. This is the note a key plays on its own, and the root of its chord.
  uint8_t noteForPad(uint8_t track, uint8_t pad) const;
  // Notes a keyboard pad plays: the one above, or the chord built on it when the track has a
  // chord shape and rootOnly is false. Writes into out and returns how many, 0 past MIDI 127.
  uint8_t notesForPad(uint8_t track, uint8_t pad, bool rootOnly, uint8_t* out) const;
  // Moves the track's keyboard an octave, stopping at the ends of the range.
  void shiftOctave(uint8_t track, int8_t delta);
  void releaseHeldNotes();
  void handleStep(UiState& state, uint8_t pad, bool pressed);
  // Writes the keys held right now onto a step as its chord.
  void writeHeldChord(const UiState& state, uint16_t step);
  void handleNote(UiState& state, uint8_t pad, bool pressed);

  Sequencer& sequencer_;
  uint32_t heldSteps_;    // bit n = step pad n held
  uint32_t editedSteps_;  // held steps that got a note, so releasing them doesn't toggle
  uint32_t heldNotes_;    // bit n = keyboard pad n held and sounding
  uint32_t rootOnlyNotes_;  // bit n = that held key was pressed with Shift: one note, no chord
  uint32_t heldSince_[kNumStepPads];  // when each held step was pressed
  uint8_t previewTrack_;  // track the held notes sound on
  PianoRoll roll_;
};

}  // namespace gx
