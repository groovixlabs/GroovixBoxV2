#include "ui/NoteMode.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

static_assert(kNumStepPads <= kMaxSteps, "every step pad needs a step");
static_assert(kNumStepPads <= 32 && kNumPads - kNumStepPads <= 32,
              "held pads are tracked in 32-bit masks");

namespace {

const uint8_t kNumKeys = kNumPads - kNumStepPads;
const uint8_t kEmptyStepLevel = 14;  // faint tint so the steps read as the track
const uint8_t kRootMarkLevel = 70;   // root pads, tinted with the track colour
const uint8_t kChordKeyLevel = 26;   // the other keys, while the track plays chords
const uint8_t kDrumBlockSize = 4;
const uint8_t kDrumLeftLevel = 60;   // the two drum blocks in different shades
const uint8_t kDrumRightLevel = 28;
const Rgb kLastStepColor = {255, 0, 0};  // the pattern's last step
const uint16_t kNoStep = 0xFFFF;

static_assert(2 * kDrumBlockSize == kGridCols && kNumKeys == 2 * kDrumBlockSize * kDrumBlockSize,
              "the keyboard holds two 4x4 drum blocks");

uint32_t bit(uint8_t n) { return static_cast<uint32_t>(1) << n; }

// Drum pads: two 4x4 blocks side by side, each rising left to right and bottom to top like a
// drum machine's pads. The left block holds the 16 notes from base, the right the next 16.
uint8_t drumNote(uint8_t base, uint8_t pad) {
  const unsigned rowFromBottom = kGridRows - 1u - pad / kGridCols;
  const unsigned col = pad % kGridCols;
  const unsigned note = base + (col / kDrumBlockSize) * kDrumBlockSize * kDrumBlockSize +
                        rowFromBottom * kDrumBlockSize + col % kDrumBlockSize;
  return note <= kMaxMidiValue ? static_cast<uint8_t>(note) : kInvalidNote;
}

// Scale degree of a keyboard pad, 0 at bottom-left and rising left to right. Rows start on
// the scale root, so a 7-note scale fills a row with one octave: the octave root sits on the
// eighth pad and again at the start of the row above. A scale with 8 or more notes per octave
// (chromatic) would lose notes that way, so its rows carry on where the last one ended and
// the roots run diagonally.
uint8_t keyDegree(uint8_t pad, uint8_t notesPerOctave) {
  const uint8_t rowStep = notesPerOctave < kGridCols ? notesPerOctave : kGridCols;
  const uint8_t rowFromBottom = static_cast<uint8_t>(kGridRows - 1 - pad / kGridCols);
  return static_cast<uint8_t>(rowFromBottom * rowStep + pad % kGridCols);
}

}  // namespace

NoteMode::NoteMode(Sequencer& sequencer)
    : sequencer_(sequencer),
      heldSteps_(0),
      editedSteps_(0),
      heldNotes_(0),
      rootOnlyNotes_(0),
      previewTrack_(0),
      roll_(sequencer) {
  for (uint8_t pad = 0; pad < kNumStepPads; ++pad) heldSince_[pad] = 0;
}

uint8_t NoteMode::noteForPad(uint8_t track, uint8_t pad) const {
  const uint8_t base = octaveBaseNote(sequencer_.keyboardOctave(track));
  if (sequencer_.keyboardLayout(track) == kKeyboardDrums) return drumNote(base, pad);
  const uint8_t scale = sequencer_.keyboardScale(track);
  const uint8_t rootNote = static_cast<uint8_t>(base + sequencer_.keyboardRoot(track));
  return scaleNote(scale, rootNote, keyDegree(pad, scaleLength(scale)));
}

// One key, one chord: the key's degree with more notes of the same scale stacked on top.
// Drum pads have no scale to stack on, and Shift asks for the single note.
uint8_t NoteMode::notesForPad(uint8_t track, uint8_t pad, bool rootOnly, uint8_t* out) const {
  const uint8_t shape = sequencer_.trackChord(track);
  if (rootOnly || shape == kChordOff ||
      sequencer_.keyboardLayout(track) == kKeyboardDrums) {
    const uint8_t note = noteForPad(track, pad);
    if (note == kInvalidNote) return 0;
    out[0] = note;
    return 1;
  }
  const uint8_t base = octaveBaseNote(sequencer_.keyboardOctave(track));
  const uint8_t scale = sequencer_.keyboardScale(track);
  const uint8_t rootNote = static_cast<uint8_t>(base + sequencer_.keyboardRoot(track));
  return chordNotes(scale, rootNote, keyDegree(pad, scaleLength(scale)), shape, out);
}

void NoteMode::shiftOctave(uint8_t track, int8_t delta) {
  const uint8_t octave = sequencer_.keyboardOctave(track);
  if (delta < 0 && octave == 0) return;
  if (delta > 0 && octave >= kMaxKeyboardOctave) return;
  releaseHeldNotes();  // the held pitches belong to the octave being left
  sequencer_.setKeyboardOctave(track, static_cast<uint8_t>(octave + delta));
}

void NoteMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (sequencer_.trackPianoRoll(state.track)) {
    roll_.handlePad(state, pad, pressed);
    return;
  }
  if (pad < kNumStepPads) {
    handleStep(state, pad, pressed);
  } else if (pad < kNumPads) {
    handleNote(state, pad, pressed);
  }
}

void NoteMode::handleStep(UiState& state, uint8_t pad, bool pressed) {
  const uint16_t step = static_cast<uint16_t>(state.stepPage * kStepsPerPage + pad);
  if (!pressed) {
    if (heldSteps_ & bit(pad)) {
      // A tap switches an empty step on. A lit step never switches off here, as on Circuit
      // Tracks: pressing it shows its notes, and only Clear + step removes it.
      const bool tap = state.nowMs - heldSince_[pad] < kTapMaxMs;
      if (tap && !(editedSteps_ & bit(pad)) && !sequencer_.stepActive(state.track, step)) {
        sequencer_.toggleStep(state.track, step);
      }
      heldSteps_ &= ~bit(pad);
      editedSteps_ &= ~bit(pad);
    }
    return;
  }

  uint16_t from = kNoSlot;
  if (state.shiftHeld) {
    // Shift + a step ends the pattern there.
    sequencer_.setTrackLength(state.track, static_cast<uint16_t>(step + 1));
  } else if (state.clearHeld) {
    sequencer_.clearStep(state.track, step);
  } else if (state.duplicateHeld) {
    if (takeDuplicateTarget(state, step, from)) {
      sequencer_.copyStep(state.track, from, step);
    }
  } else if (heldNotes_) {
    // Keys are held: the step takes them as its chord.
    writeHeldChord(state, step);
    heldSteps_ |= bit(pad);
    editedSteps_ |= bit(pad);  // it has its notes, so the release must not toggle it off
  } else {
    // Switching on waits for the release, so a held step can show its notes or take keys.
    heldSteps_ |= bit(pad);
    heldSince_[pad] = state.nowMs;
  }
}

void NoteMode::writeHeldChord(const UiState& state, uint16_t step) {
  uint8_t notes[kMaxStepNotes];
  uint8_t count = 0;
  for (uint8_t key = 0; key < kNumKeys && count < kMaxStepNotes; ++key) {
    if (!(heldNotes_ & bit(key))) continue;
    uint8_t keyNotes[kMaxStepNotes];
    const uint8_t n = notesForPad(state.track, static_cast<uint8_t>(kNumStepPads + key),
                                  (rootOnlyNotes_ & bit(key)) != 0, keyNotes);
    // A chord key already fills the step, and two keys can offer the same note - the rows
    // repeat the root, and chords a third apart share notes - so keep the first of each.
    for (uint8_t i = 0; i < n && count < kMaxStepNotes; ++i) {
      bool seen = false;
      for (uint8_t have = 0; have < count; ++have) seen = seen || notes[have] == keyNotes[i];
      if (!seen) notes[count++] = keyNotes[i];
    }
  }
  if (count) sequencer_.setStepChord(state.track, step, notes, count);
}

void NoteMode::handleNote(UiState& state, uint8_t pad, bool pressed) {
  const uint32_t mask = bit(static_cast<uint8_t>(pad - kNumStepPads));

  if (!pressed) {
    if (heldNotes_ & mask) {
      uint8_t notes[kMaxStepNotes];
      const uint8_t count =
          notesForPad(previewTrack_, pad, (rootOnlyNotes_ & mask) != 0, notes);
      for (uint8_t n = 0; n < count; ++n) sequencer_.previewNoteOff(previewTrack_, notes[n]);
      heldNotes_ &= ~mask;
      rootOnlyNotes_ &= ~mask;
    }
    return;
  }

  // Shift + the outer keyboard columns moves the keyboard an octave; it plays no note. On any
  // other key it asks for the single note, which is how you play one note of a chord track.
  bool rootOnly = false;
  if (state.shiftHeld) {
    const uint8_t column = pad % kGridCols;
    if (column == 0) {
      shiftOctave(state.track, -1);
      return;
    }
    if (column == kGridCols - 1) {
      shiftOctave(state.track, 1);
      return;
    }
    rootOnly = true;
  }

  uint8_t notes[kMaxStepNotes];
  const uint8_t count = notesForPad(state.track, pad, rootOnly, notes);
  if (count == 0) return;  // past the top of the MIDI range

  previewTrack_ = state.track;
  heldNotes_ |= mask;
  if (rootOnly) rootOnlyNotes_ |= mask;
  for (uint8_t n = 0; n < count; ++n) {
    sequencer_.previewNoteOn(state.track, notes[n], state.padVelocity);
  }

  if (heldSteps_) {
    for (uint8_t s = 0; s < kNumStepPads; ++s) {
      if (!(heldSteps_ & bit(s))) continue;
      const uint16_t step = static_cast<uint16_t>(state.stepPage * kStepsPerPage + s);
      // A key toggles its notes on a held step: the chord goes on if its root isn't there
      // already, and comes off if it is, so one key puts a chord in and takes it out again.
      const bool remove = sequencer_.stepHasNote(state.track, step, notes[0]);
      for (uint8_t n = 0; n < count; ++n) {
        if (remove) {
          sequencer_.removeStepNote(state.track, step, notes[n]);
        } else {
          sequencer_.addStepNote(state.track, step, notes[n]);
        }
      }
    }
    editedSteps_ |= heldSteps_;
  } else {
    // All of these land on the same step: nothing moves the playhead between them, and one
    // press means one velocity for the chord it wrote.
    for (uint8_t n = 0; n < count; ++n) {
      sequencer_.recordNote(state.track, notes[n], state.padVelocity);
    }
  }
}

void NoteMode::releaseHeldNotes() {
  for (uint8_t key = 0; key < kNumKeys; ++key) {
    if (!(heldNotes_ & bit(key))) continue;
    const uint8_t pad = static_cast<uint8_t>(kNumStepPads + key);
    uint8_t notes[kMaxStepNotes];
    const uint8_t count =
        notesForPad(previewTrack_, pad, (rootOnlyNotes_ & bit(key)) != 0, notes);
    for (uint8_t n = 0; n < count; ++n) sequencer_.previewNoteOff(previewTrack_, notes[n]);
  }
  heldNotes_ = 0;
  rootOnlyNotes_ = 0;
}

void NoteMode::reset() {
  roll_.reset();
  releaseHeldNotes();
  heldSteps_ = 0;
  editedSteps_ = 0;
}

void NoteMode::renderPads(const UiState& state, LedFrame& frame) const {
  if (sequencer_.trackPianoRoll(state.track)) {
    roll_.render(state, frame);
    return;
  }
  const uint8_t track = state.track;
  const Rgb color = trackColor(track);
  const uint16_t length = sequencer_.trackLength(track);
  const bool playing = sequencer_.playing();
  const uint16_t playhead = sequencer_.playhead(track);

  // The top four rows show one page of steps; hold R3 and press B1..B8 for another page.
  const uint16_t firstStep = static_cast<uint16_t>(state.stepPage * kStepsPerPage);
  uint16_t heldStep = kNoStep;
  for (uint8_t pad = 0; pad < kNumStepPads; ++pad) {
    const uint16_t step = static_cast<uint16_t>(firstStep + pad);
    const bool active = sequencer_.stepActive(track, step);
    if (step >= length) {
      frame.pads[pad] = kBlack;
    } else if (heldSteps_ & bit(pad)) {
      frame.pads[pad] = kSelectedColor;
      if (heldStep == kNoStep) heldStep = step;
    } else if (state.duplicateHeld && step == state.duplicateSource) {
      frame.pads[pad] = kCopySourceColor;
    } else if (playing && step == playhead) {
      frame.pads[pad] = playheadColor(sequencer_.recording(), active);
    } else if (step + 1 == length) {
      frame.pads[pad] = kLastStepColor;  // Shift + a step moves the end here
    } else {
      frame.pads[pad] = active ? color : dim(color, kEmptyStepLevel);
    }
  }

  // Keyboard: root pads tinted so octaves are easy to find; pads past MIDI 127 stay dark.
  const uint8_t notesPerOctave = scaleLength(sequencer_.keyboardScale(track));
  const bool drums = sequencer_.keyboardLayout(track) == kKeyboardDrums;
  const bool chords = !drums && sequencer_.trackChord(track) != kChordOff;
  for (uint8_t pad = kNumStepPads; pad < kNumPads; ++pad) {
    if (noteForPad(track, pad) == kInvalidNote) {
      frame.pads[pad] = kBlack;
    } else if (drums) {
      const bool left = pad % kGridCols < kDrumBlockSize;
      frame.pads[pad] = dim(color, left ? kDrumLeftLevel : kDrumRightLevel);
    } else {
      // A chord track's keys carry a hint of the track colour, so it is clear before you play
      // a note that one key is a chord here.
      const Rgb key = chords ? dim(color, kChordKeyLevel) : kEmptyColor;
      frame.pads[pad] = (keyDegree(pad, notesPerOctave) % notesPerOctave == 0)
                            ? dim(color, kRootMarkLevel)
                            : key;
    }
  }

  // Show every note of the held step in green, otherwise those of the step being played in
  // the track colour. Rows repeat the root, so one note can light more than one pad.
  uint16_t shownStep = kNoStep;
  Rgb shownColor = kSelectedColor;
  if (heldStep != kNoStep) {
    shownStep = heldStep;
  } else if (playing && sequencer_.stepActive(track, playhead)) {
    shownStep = playhead;
    shownColor = color;
  }
  if (shownStep != kNoStep) {
    const uint8_t count = sequencer_.stepNoteCount(track, shownStep);
    for (uint8_t n = 0; n < count; ++n) {
      const uint8_t note = sequencer_.stepNote(track, shownStep, n);
      for (uint8_t pad = kNumStepPads; pad < kNumPads; ++pad) {
        if (noteForPad(track, pad) == note) frame.pads[pad] = shownColor;
      }
    }
  }

  for (uint8_t key = 0; key < kNumKeys; ++key) {
    if (heldNotes_ & bit(key)) frame.pads[kNumStepPads + key] = kWhite;
  }

  // A key held on a MIDI keyboard lights the pad that plays the same note, so what you play
  // is visible on the grid even though your hands are somewhere else. Rows repeat the root,
  // so one note can light more than one pad, as a held step's notes do.
  for (uint8_t i = 0; i < state.playingNoteCount; ++i) {
    for (uint8_t pad = kNumStepPads; pad < kNumPads; ++pad) {
      if (noteForPad(track, pad) == state.playingNotes[i]) frame.pads[pad] = kWhite;
    }
  }
}

}  // namespace gx
