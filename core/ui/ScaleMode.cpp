#include "ui/ScaleMode.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kNone = 0xFF;
const uint8_t kBlackKeyRow = 0;
const uint8_t kWhiteKeyRow = 1;
const uint8_t kFirstScalePad = 4 * kGridCols;  // the bottom four rows

// Root (semitone above C) of each pad on the piano rows, left to right.
const uint8_t kBlackKeys[kGridCols] = {kNone, 1, 3, kNone, 6, 8, 10, kNone};
const uint8_t kWhiteKeys[kGridCols] = {0, 2, 4, 5, 7, 9, 11, 0};

const uint8_t kWhiteKeyLevel = 70;  // piano keys glow lightly; black keys dimmer
const uint8_t kBlackKeyLevel = 45;

// Bottom row, last two pads: the selected track's keyboard.
const uint8_t kDrumTrackPad = kNumPads - 2;
const uint8_t kOwnScalePad = kNumPads - 1;
// The last four pads say what the keys play. The piano roll is not one of them: it puts a
// different grid on screen altogether, so it sits alone at the other end of the row with the
// dark pads between as a gap.
const uint8_t kPianoRollPad = kNumPads - kGridCols;
// One key, one chord: a pad each, so the APC shows the setting the way it shows every other
// one on this row - lit is the shape in use, both dim is off. Tapping the lit one turns it off.
const uint8_t kSeventhPad = kNumPads - 3;
const uint8_t kTriadPad = kNumPads - 4;
const uint8_t kLayoutOffLevel = 30;
const Rgb kDrumLayoutColor = {255, 150, 0};  // amber, so Drums can't pass for Own scale
const Rgb kChordColor = {160, 60, 255};      // violet: nothing else on the row is near it
const uint8_t kSongHintLevel = 90;  // the song's key, faintly, while a track has its own

// Pad labels: whole names where they fit, broken over lines of up to kPadLabelLineChars.
const char* const kScalePadLabels[kNumScales] = {
    "MAJOR",         "MINOR",          "DORIAN",       "PHRY-\nGIAN",  "LYDIAN",
    "MIXO-\nLYDIAN", "LOCRIAN",        "HARM\nMINOR",  "MELODIC\nMINOR", "MAJOR\nPENTA",
    "MINOR\nPENTA",  "BLUES",          "WHOLE\nTONE",  "DIMIN-\nISHED", "PHRYG\nDOM",
    "HUNGA-\nRIAN",  "DOUBLE\nHARM",   "LYDIAN\nDOM",  "ALTERED",      "NEAPO-\nLITAN",
    "BEBOP\nDOM",    "HIRA-\nJOSHI",   "IN SEN",       "CHRO-\nMATIC",
};

uint8_t rootForPad(uint8_t pad) {
  const uint8_t row = pad / kGridCols;
  if (row == kBlackKeyRow) return kBlackKeys[pad % kGridCols];
  if (row == kWhiteKeyRow) return kWhiteKeys[pad % kGridCols];
  return kNone;
}

uint8_t scaleForPad(uint8_t pad) {
  if (pad < kFirstScalePad || pad - kFirstScalePad >= kNumScales) return kNone;
  return static_cast<uint8_t>(pad - kFirstScalePad);
}

// The keyboard layout a pad switches to, or kNone.
uint8_t layoutForPad(uint8_t pad) {
  if (pad == kDrumTrackPad) return kKeyboardDrums;
  if (pad == kOwnScalePad) return kKeyboardOwnScale;
  return kNone;
}

// The chord shape a pad switches to, or kNone.
uint8_t chordForPad(uint8_t pad) {
  if (pad == kTriadPad) return kChordTriad;
  if (pad == kSeventhPad) return kChordSeventh;
  return kNone;
}

}  // namespace

static_assert(kFirstScalePad + kNumScales <= kPianoRollPad,
              "every scale needs a pad, clear of the track's bottom row");

ScaleMode::ScaleMode(Sequencer& sequencer) : sequencer_(sequencer) {}

void ScaleMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (!pressed) return;
  const uint8_t track = state.track;
  if (pad == kPianoRollPad) {
    sequencer_.setTrackPianoRoll(track, !sequencer_.trackPianoRoll(track));
    return;
  }
  const uint8_t shape = chordForPad(pad);
  if (shape != kNone) {
    // Tapping the shape in use turns chords off, the way the layout pads work.
    const bool inUse = sequencer_.trackChord(track) == shape;
    sequencer_.setTrackChord(track, inUse ? static_cast<uint8_t>(kChordOff) : shape);
    return;
  }
  const uint8_t layout = layoutForPad(pad);
  if (layout != kNone) {
    // Tapping the layout in use goes back to the song's scale.
    const bool inUse = sequencer_.keyboardLayout(track) == layout;
    sequencer_.setKeyboardLayout(track,
                                 inUse ? static_cast<uint8_t>(kKeyboardProjectScale) : layout);
    return;
  }

  // A track with its own scale edits that; otherwise the picks set the song's key.
  const bool own = sequencer_.keyboardLayout(track) == kKeyboardOwnScale;
  const uint8_t root = rootForPad(pad);
  const uint8_t scale = scaleForPad(pad);
  if (root != kNone) {
    if (own) {
      sequencer_.setOwnScaleRoot(track, root);
    } else {
      sequencer_.setScaleRoot(root);
    }
  } else if (scale != kNone) {
    if (own) {
      sequencer_.setOwnScale(track, scale);
    } else {
      sequencer_.setScale(scale);
    }
  }
}

const char* ScaleMode::padLabel(const UiState&, uint8_t pad) const {
  const uint8_t root = rootForPad(pad);
  if (root != kNone) return rootName(root);
  const uint8_t scale = scaleForPad(pad);
  if (scale != kNone) return kScalePadLabels[scale];
  if (pad == kTriadPad) return "TRIAD";
  if (pad == kSeventhPad) return "7TH";
  if (pad == kPianoRollPad) return "PIANO\nROLL";
  if (pad == kDrumTrackPad) return "DRUMS";
  if (pad == kOwnScalePad) return "OWN\nSCALE";
  return nullptr;
}

void ScaleMode::renderPads(const UiState& state, LedFrame& frame) const {
  const uint8_t track = state.track;
  const Rgb color = trackColor(track);
  const uint8_t layout = sequencer_.keyboardLayout(track);
  // The song's key is green. While the track has its own scale, that scale lights in the
  // track colour and the song's key stays as a faint green hint, so both can be seen.
  const bool own = layout == kKeyboardOwnScale;
  const Rgb songColor = own ? dim(kSelectedColor, kSongHintLevel) : kSelectedColor;

  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    const uint8_t root = rootForPad(pad);
    const uint8_t scale = scaleForPad(pad);
    const uint8_t padLayout = layoutForPad(pad);
    const uint8_t padChord = chordForPad(pad);
    if (root != kNone) {
      const uint8_t level = (pad / kGridCols == kBlackKeyRow) ? kBlackKeyLevel : kWhiteKeyLevel;
      Rgb c = dim(kWhite, level);
      if (root == sequencer_.scaleRoot()) c = songColor;
      if (own && root == sequencer_.keyboardRoot(track)) c = color;
      frame.pads[pad] = c;
    } else if (scale != kNone) {
      Rgb c = kFilledColor;
      if (scale == sequencer_.scale()) c = songColor;
      if (own && scale == sequencer_.keyboardScale(track)) c = color;
      frame.pads[pad] = c;
    } else if (pad == kPianoRollPad) {
      frame.pads[pad] =
          sequencer_.trackPianoRoll(track) ? kWhite : dim(kWhite, kLayoutOffLevel);
    } else if (padChord != kNone) {
      frame.pads[pad] = (sequencer_.trackChord(track) == padChord)
                            ? kChordColor
                            : dim(kChordColor, kLayoutOffLevel);
    } else if (padLayout != kNone) {
      const Rgb on = padLayout == kKeyboardDrums ? kDrumLayoutColor : color;
      frame.pads[pad] = (layout == padLayout) ? on : dim(on, kLayoutOffLevel);
    } else {
      frame.pads[pad] = kBlack;
    }
  }
}

}  // namespace gx
