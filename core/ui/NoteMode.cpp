#include "ui/NoteMode.h"

#include "ui/Controls.h"
#include "engine/Scale.h"
#include "ui/Palette.h"

namespace gx {

static_assert(kNumStepPads <= kMaxSteps, "every step pad needs a step");
static_assert(kNumStepPads <= 32 && kNumPads - kNumStepPads <= 32,
              "held pads are tracked in 32-bit masks");

namespace {

const uint8_t kNumKeys = kNumPads - kNumStepPads;
// Shift + the top-left and bottom-left keys move the keyboard an octave. Vertical rather
// than the outer columns it used to be, because pitch runs up this keyboard - every row is
// higher than the one below it - so moving by an octave should go the same way the notes do.
// Two pads rather than whole edges, so Shift + a key still asks for the single note almost
// everywhere, which is how one note of a chord track is played.
const uint8_t kOctaveUpPad = kNumStepPads;                     // row 5, column 1
const uint8_t kOctaveDownPad = kNumPads - kGridCols;           // row 8, column 1
// The modes whose pads change wholesale under Shift - the mutes, the preset windows, the
// ratchet lane - already repaint and need no hint on top. See shiftHintColor().
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
  if (state.noteHeld) {
    // R1 held + a step ends the pattern there. Shift used to do this, but Shift on the top
    // row now picks a step page, and paging happens constantly while a length is set once -
    // so the frequent gesture gets the easy modifier and this one takes R1.
    sequencer_.setTrackLength(state.track, static_cast<uint16_t>(step + 1));
  } else if (state.shiftHeld) {
    // Shift alone on a step does nothing now: better inert than toggling a step because a
    // thumb was still on Shift.
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

  // Shift + the keyboard's top-left and bottom-left keys move it an octave; they play no
  // note. On any other key Shift asks for the single note, which is how you play one note of
  // a chord track.
  bool rootOnly = false;
  if (state.shiftHeld) {
    if (pad == kOctaveUpPad) {
      shiftOctave(state.track, 1);
      return;
    }
    if (pad == kOctaveDownPad) {
      shiftOctave(state.track, -1);
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

  // The top four rows show one page of steps; hold R1 and press B1..B8 for another page.
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
      frame.pads[pad] = kLastStepColor;  // R1 held + a step moves the end here
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

  // Last, so it sits over whatever those keys were showing: Shift makes its two octave keys
  // plain, since nothing else on the grid says they are there.
  if (state.shiftHeld) {
    frame.pads[kOctaveUpPad] = shiftHintColor();
    frame.pads[kOctaveDownPad] = shiftHintColor();
  }
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kStepsColor = {54, 214, 95};
const Rgb kKeysColor = {0, 215, 200};
const Rgb kOctaveColor = {255, 189, 108};

const ModeLegend kNoteLegend = {
    "R1",
    "Note",
    {
        {1, 4, kStepsColor, "rows 1-4", "the 32 steps - R5 + step removes one"},
        {5, 8, kKeysColor, "rows 5-8", "the keyboard, from the root at left"},
        {0, 0, kLastStepColor, "R1 + step", "makes it the pattern's last step"},
        {0, 0, kOctaveColor, "SHIFT key", "top left up, bottom left down an octave"},
        {0, 0, kSelectedColor, "SHIFT top", "row picks the page of 32 steps"},
        {0, 0, {}, NULL, NULL},
    },
    5,
    {"hold step + key give it that chord", "SHIFT + R1 scale", NULL, NULL},
    2,
    {kStepsColor, kStepsColor, kStepsColor, kStepsColor,
     kKeysColor, kKeysColor, kKeysColor, kKeysColor},
};

// The same mode showing a track as a piano roll: one grid, not two halves.
const ModeLegend kRollLegend = {
    "R1",
    "Note - piano roll",
    {
        {1, 8, kKeysColor, "the grid", "8 steps across, 8 scale notes up"},
        {0, 0, kLastStepColor, "R1 + pad", "makes that step the pattern's last"},
        {0, 0, shiftHintColor(), "SHIFT arrow", "bottom right: scrolls the view by 1"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"tap a pad add or remove that note", "hold R1 + B1-B8 step page", "SHIFT + R1 scale", NULL},
    3,
    {kKeysColor, kKeysColor, kKeysColor, kKeysColor,
     kKeysColor, kKeysColor, kKeysColor, kKeysColor},
};

}  // namespace

// The roll is the same mode with a different grid on it, so it says something different -
// and which one is per track, so the legend is asked for with the state to hand.
const ModeLegend* NoteMode::legend(const UiState& state) const {
  return sequencer_.trackPianoRoll(state.track) ? &kRollLegend : &kNoteLegend;
}

uint8_t NoteMode::displayValues(const UiState& state, DisplayValue* values) const {
  const bool own = sequencer_.keyboardLayout(state.track) == kKeyboardOwnScale;
  const bool drums = sequencer_.keyboardLayout(state.track) == kKeyboardDrums;
  uint8_t count = 0;
  values[count].key = "KEY";
  values[count].text = drums ? "drum pads"
                             : rootName(own ? sequencer_.keyboardRoot(state.track)
                                            : sequencer_.scaleRoot());
  values[count].number = 0;
  values[count].suffix = drums ? NULL
                               : scaleName(own ? sequencer_.keyboardScale(state.track)
                                               : sequencer_.scale());
  ++count;
  values[count].key = "OCTAVE";
  values[count].text = NULL;
  values[count].number = sequencer_.keyboardOctave(state.track);
  values[count].suffix = NULL;
  ++count;
  values[count].key = "LENGTH";
  values[count].text = NULL;
  values[count].number = sequencer_.trackLength(state.track);
  values[count].suffix = " steps";
  ++count;
  return count;
}

}  // namespace gx
