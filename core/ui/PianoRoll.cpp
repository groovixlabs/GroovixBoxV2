#include "ui/PianoRoll.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kScrollBy = 4;            // half the view, so what was on screen stays in sight
const uint8_t kInactiveNoteLevel = 45;  // notes of a switched-off step
const uint8_t kPastEndNoteLevel = 20;   // notes of steps past the pattern's end
const uint8_t kRootRowLevel = 22;       // the scale root's rows, tinted with the track colour
const uint8_t kEmptyLevel = 14;         // every other empty pad
const uint8_t kLastStepLevel = 50;      // the last step's column
const Rgb kLastStepColor = {255, 0, 0};
const int kMaxDegree = 255 - (kGridRows - 1);  // scaleNote takes degrees up to 255

}  // namespace

static_assert(kMaxSteps >= kGridCols, "the piano roll shows 8 steps");

PianoRoll::PianoRoll(Sequencer& sequencer) : sequencer_(sequencer), heldTrack_(0) {
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    firstSteps_[t] = kNoSlot;
    noteOffsets_[t] = 0;
  }
  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    heldNotes_[pad] = kInvalidNote;
    heldSteps_[pad] = 0;
    heldSince_[pad] = 0;
  }
}

void PianoRoll::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (pad >= kNumPads) return;
  if (!pressed) {
    if (heldNotes_[pad] != kInvalidNote) {
      const uint8_t note = heldNotes_[pad];
      sequencer_.previewNoteOff(heldTrack_, note);
      heldNotes_[pad] = kInvalidNote;
      // A tap adds or removes the note; a longer hold only played it.
      if (state.nowMs - heldSince_[pad] < kTapMaxMs) toggleNote(heldTrack_, heldSteps_[pad], note);
    }
    return;
  }

  const uint8_t track = state.track;
  const uint8_t row = pad / kGridCols;
  const uint16_t step = static_cast<uint16_t>(firstStep(state) + pad % kGridCols);
  uint16_t from = kNoSlot;
  if (state.shiftHeld) {
    // Shift + a bottom-row pad ends the pattern at its step, as Shift + a step does.
    if (row == kGridRows - 1) sequencer_.setTrackLength(track, static_cast<uint16_t>(step + 1));
    return;
  }
  if (state.clearHeld) {
    sequencer_.clearStep(track, step);
    return;
  }
  if (state.duplicateHeld) {
    if (takeDuplicateTarget(state, step, from)) sequencer_.copyStep(track, from, step);
    return;
  }

  const uint8_t note = noteForDegree(track, bottomDegree(track) + (kGridRows - 1 - row));
  if (note == kInvalidNote) return;  // past the top of the MIDI range
  // The note plays while the pad is held; the step changes on release, and only for a tap.
  heldTrack_ = track;
  heldNotes_[pad] = note;
  heldSteps_[pad] = step;
  heldSince_[pad] = state.nowMs;
  sequencer_.previewNoteOn(track, note);
}

void PianoRoll::toggleNote(uint8_t track, uint16_t step, uint8_t note) {
  if (sequencer_.stepHasNote(track, step, note)) {
    if (sequencer_.stepActive(track, step)) {
      sequencer_.removeStepNote(track, step, note);
    } else {
      sequencer_.toggleStep(track, step);  // a switched-off step comes back with its chord
    }
  } else {
    sequencer_.addStepNote(track, step, note);
  }
}

void PianoRoll::render(const UiState& state, LedFrame& frame) const {
  const uint8_t track = state.track;
  const Rgb color = trackColor(track);
  const uint16_t length = sequencer_.trackLength(track);
  const bool playing = sequencer_.playing();
  const uint16_t playhead = sequencer_.playhead(track);
  const uint16_t first = firstStep(state);
  const int bottom = bottomDegree(track);
  const uint8_t notesPerOctave = scaleLength(sequencer_.keyboardScale(track));

  for (uint8_t row = 0; row < kGridRows; ++row) {
    const int degree = bottom + (kGridRows - 1 - row);
    const uint8_t note = noteForDegree(track, degree);
    const bool rootRow = degree % notesPerOctave == 0;
    for (uint8_t col = 0; col < kGridCols; ++col) {
      const uint8_t pad = padIndex(row, col);
      if (note == kInvalidNote) {
        frame.pads[pad] = kBlack;
        continue;
      }
      const uint16_t step = static_cast<uint16_t>(first + col);
      const bool inChord = sequencer_.stepHasNote(track, step, note);
      const bool active = inChord && sequencer_.stepActive(track, step);
      Rgb c;
      if (step >= length) {
        c = inChord ? dim(color, kPastEndNoteLevel) : kBlack;
      } else if (playing && step == playhead) {
        c = playheadColor(sequencer_.recording(), active);
      } else if (inChord) {
        c = active ? color : dim(color, kInactiveNoteLevel);
      } else if (step + 1 == length) {
        c = dim(kLastStepColor, kLastStepLevel);
      } else {
        c = rootRow ? dim(color, kRootRowLevel) : dim(kWhite, kEmptyLevel);
      }
      if (heldNotes_[pad] != kInvalidNote) c = kWhite;
      frame.pads[pad] = c;
    }
  }
}

void PianoRoll::scroll(UiState& state, uint8_t direction) {
  if (!canScroll(state, direction)) return;
  const uint8_t track = state.track;
  switch (direction) {
    case kRollUp:
      noteOffsets_[track] = static_cast<int16_t>(noteOffsets_[track] + kScrollBy);
      break;
    case kRollDown: {
      // Stop at the lowest note instead of overshooting, so scrolling up comes back.
      const int bottom = bottomDegree(track);
      const int by = bottom < kScrollBy ? bottom : kScrollBy;
      noteOffsets_[track] = static_cast<int16_t>(noteOffsets_[track] - by);
      break;
    }
    case kRollLeft:
      setFirstStep(state, firstStep(state) - kScrollBy);
      break;
    case kRollRight:
      setFirstStep(state, firstStep(state) + kScrollBy);
      break;
  }
  reset();  // held pads now sit on other notes
}

bool PianoRoll::canScroll(const UiState& state, uint8_t direction) const {
  const uint8_t track = state.track;
  switch (direction) {
    case kRollUp:
      return noteForDegree(track, bottomDegree(track) + kScrollBy) != kInvalidNote;
    case kRollDown:
      return bottomDegree(track) > 0;
    case kRollLeft:
      return firstStep(state) > 0;
    case kRollRight:
      return firstStep(state) < kMaxSteps - kGridCols;
    default:
      return false;
  }
}

void PianoRoll::showStepPage(UiState& state, uint8_t page) {
  setFirstStep(state, page * kStepsPerPage);
  reset();
}

void PianoRoll::reset() {
  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    if (heldNotes_[pad] != kInvalidNote) {
      sequencer_.previewNoteOff(heldTrack_, heldNotes_[pad]);
      heldNotes_[pad] = kInvalidNote;
    }
  }
}

uint16_t PianoRoll::firstStep(const UiState& state) const {
  const uint16_t stored = firstSteps_[state.track];
  const unsigned first = stored != kNoSlot ? stored : state.stepPage * kStepsPerPage;
  const unsigned last = kMaxSteps - kGridCols;
  return static_cast<uint16_t>(first > last ? last : first);
}

// The bottom row: the scale root in the track's keyboard octave, moved by any scrolling.
int PianoRoll::bottomDegree(uint8_t track) const {
  const int degree = scaleLength(sequencer_.keyboardScale(track)) *
                         (sequencer_.keyboardOctave(track) + 1) +
                     noteOffsets_[track];
  if (degree < 0) return 0;
  return degree > kMaxDegree ? kMaxDegree : degree;
}

uint8_t PianoRoll::noteForDegree(uint8_t track, int degree) const {
  if (degree < 0 || degree > 255) return kInvalidNote;
  return scaleNote(sequencer_.keyboardScale(track), sequencer_.keyboardRoot(track),
                   static_cast<uint8_t>(degree));
}

void PianoRoll::setFirstStep(UiState& state, int first) {
  const int last = kMaxSteps - kGridCols;
  if (first < 0) first = 0;
  if (first > last) first = last;
  firstSteps_[state.track] = static_cast<uint16_t>(first);
  state.stepPage = static_cast<uint8_t>(first / kStepsPerPage);  // step page LEDs follow the view
}

}  // namespace gx
