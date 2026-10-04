#include "ui/SlotModes.h"

#include "engine/FactoryPatterns.h"
#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

static_assert(kNumTracks * kNumPatterns < kNoSlot, "pattern slots fit in 16 bits");

namespace {

// Pattern mode slots number every pattern of every track: track * kNumPatterns + pattern.
uint8_t slotTrack(uint16_t slot) { return static_cast<uint8_t>(slot / kNumPatterns); }
uint8_t slotPattern(uint16_t slot) { return static_cast<uint8_t>(slot % kNumPatterns); }

uint16_t pagedSlot(uint8_t page, uint8_t pad) {
  return static_cast<uint16_t>(page * kSlotsPerPage + pad);
}

}  // namespace

// ---- SlotGridMode ----

// Slots that hold nothing have nothing to clear or copy, so a modifier held over them does
// nothing at all - it doesn't quietly fall through to picking the slot either, since the
// press was meant as an edit.
bool SlotGridMode::hasData(const UiState&, uint16_t) const { return false; }
Rgb SlotGridMode::filledColor(const UiState&, uint16_t) const { return kFilledColor; }
void SlotGridMode::clear(UiState&, uint16_t) {}
void SlotGridMode::copy(UiState&, uint16_t, uint16_t) {}

void SlotGridMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (!pressed) return;
  const uint16_t slot = slotForPad(state, pad);
  if (slot == kNoSlot) return;
  if (state.clearHeld || state.duplicateHeld) {
    if (!slotsHoldData()) return;
    uint16_t from = kNoSlot;
    if (state.clearHeld) {
      clear(state, slot);
    } else if (takeDuplicateTarget(state, slot, from)) {
      copy(state, from, slot);
    }
    return;
  }
  select(state, slot);
}

void SlotGridMode::renderPads(const UiState& state, LedFrame& frame) const {
  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    const uint16_t slot = slotForPad(state, pad);
    if (slot == kNoSlot) {
      frame.pads[pad] = kBlack;
    } else if (state.duplicateHeld && slot == state.duplicateSource) {
      frame.pads[pad] = kCopySourceColor;
    } else if (isSelected(state, slot)) {
      frame.pads[pad] = kSelectedColor;
    } else if (hasData(state, slot)) {
      frame.pads[pad] = filledColor(state, slot);
    } else {
      frame.pads[pad] = kEmptyColor;
    }
  }
}

// ---- ProjectMode ----

ProjectMode::ProjectMode(Library& library) : library_(library) {}

uint16_t ProjectMode::slotForPad(const UiState& state, uint8_t pad) const {
  return pagedSlot(state.projectPage, pad);
}

bool ProjectMode::isSelected(const UiState&, uint16_t slot) const {
  return library_.currentProject() == slot;
}

bool ProjectMode::hasData(const UiState&, uint16_t slot) const {
  return library_.projectHasData(slot);
}

void ProjectMode::select(UiState&, uint16_t slot) { library_.selectProject(slot); }

void ProjectMode::clear(UiState&, uint16_t slot) { library_.clearProject(slot); }

void ProjectMode::copy(UiState&, uint16_t from, uint16_t to) { library_.copyProject(from, to); }

// ---- PatternMode ----

PatternMode::PatternMode(Sequencer& sequencer) : sequencer_(sequencer) {}

namespace {

const uint8_t kMuteRow = kGridRows - 1;
const uint8_t kMutedColumnLevel = 70;  // how far a muted track's column drops
const uint8_t kMutedRowLevel = 20;     // the mute row itself, while Shift is held

// The track a column shows, or kNumTracks past the end of the track page.
uint16_t trackForColumn(const UiState& state, uint8_t column) {
  return state.trackPage * kTracksPerPage + column;
}

}  // namespace

void PatternMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  // Shift + the bottom row mutes, the same row scene mode uses for it.
  if (pressed && state.shiftHeld && pad / kGridCols == kMuteRow) {
    const uint16_t track = trackForColumn(state, pad % kGridCols);
    if (track < kNumTracks) {
      const uint8_t t = static_cast<uint8_t>(track);
      sequencer_.setTrackMuted(t, !sequencer_.trackMuted(t));
    }
    return;
  }
  SlotGridMode::handlePad(state, pad, pressed);
}

void PatternMode::renderPads(const UiState& state, LedFrame& frame) const {
  SlotGridMode::renderPads(state, frame);

  for (uint8_t column = 0; column < kGridCols; ++column) {
    const uint16_t track = trackForColumn(state, column);
    if (track >= kNumTracks || !sequencer_.trackMuted(static_cast<uint8_t>(track))) continue;
    for (uint8_t row = 0; row < kGridRows; ++row) {
      const uint8_t pad = padIndex(row, column);
      frame.pads[pad] = dim(frame.pads[pad], kMutedColumnLevel);
    }
  }

  // Holding Shift turns the bottom row into the mutes, as it turns the buttons into page maps.
  if (!state.shiftHeld) return;
  for (uint8_t column = 0; column < kGridCols; ++column) {
    const uint16_t track = trackForColumn(state, column);
    if (track >= kNumTracks) continue;
    const uint8_t t = static_cast<uint8_t>(track);
    const Rgb color = trackColor(t);
    frame.pads[padIndex(kMuteRow, column)] =
        sequencer_.trackMuted(t) ? dim(color, kMutedRowLevel) : color;
  }
}

uint16_t PatternMode::slotForPad(const UiState& state, uint8_t pad) const {
  const uint16_t track = state.trackPage * kTracksPerPage + pad % kGridCols;
  const uint16_t pattern = state.patternPage * kPatternsPerPage + pad / kGridCols;
  if (track >= kNumTracks || pattern >= kNumPatterns) return kNoSlot;
  return static_cast<uint16_t>(track * kNumPatterns + pattern);
}

bool PatternMode::isSelected(const UiState&, uint16_t slot) const {
  return sequencer_.selectedPattern(slotTrack(slot)) == slotPattern(slot);
}

bool PatternMode::hasData(const UiState&, uint16_t slot) const {
  return sequencer_.patternHasData(slotTrack(slot), slotPattern(slot));
}

// Amber for the built-in bank, blue for your own: the two take different gestures, so they
// should not look the same while you are deciding which pad to hit.
Rgb PatternMode::filledColor(const UiState&, uint16_t slot) const {
  return isFactoryPattern(slotPattern(slot)) ? kFactoryColor : kFilledColor;
}

// Launching a pattern also selects its track, so the column you touch is the one R3 opens
// and the one global settings edits: the bottom row pages the grid here instead of picking
// tracks.
void PatternMode::select(UiState& state, uint16_t slot) {
  const uint8_t track = slotTrack(slot);
  sequencer_.selectPattern(track, slotPattern(slot));
  state.track = track;
}

void PatternMode::clear(UiState&, uint16_t slot) {
  sequencer_.clearPattern(slotTrack(slot), slotPattern(slot));
}

void PatternMode::copy(UiState&, uint16_t from, uint16_t to) {
  sequencer_.copyPattern(slotTrack(from), slotPattern(from), slotTrack(to), slotPattern(to));
}

// ---- PresetMode ----

namespace {

// The pages of a window, 8 of 64: what the grid reaches without changing window.
const uint16_t kSlotsPerWindow = kNumPages * kSlotsPerPage;
const uint8_t kWindowRow = kGridRows - 1;  // Shift + the bottom row, as pattern mode mutes
const char* const kWindowLabels[kGridCols] = {"W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8"};

}  // namespace

PresetMode::PresetMode(Sequencer& sequencer) : sequencer_(sequencer), catalog_(NULL) {}

uint8_t PresetMode::windowOf(uint16_t slot) {
  const uint16_t window = static_cast<uint16_t>(slot / kSlotsPerWindow);
  return window < kGridCols ? static_cast<uint8_t>(window) : 0;
}

uint8_t PresetMode::pageInWindow(uint16_t slot) {
  return static_cast<uint8_t>((slot % kSlotsPerWindow) / kSlotsPerPage);
}

uint16_t PresetMode::slotCount(const UiState& state) const {
  if (!catalog_) return 0;  // nothing has said, so nothing is ruled out
  return catalog_->presetCount(sequencer_.trackMidiPort(state.track));
}

bool PresetMode::windowHasVoices(const UiState& state, uint8_t window) const {
  const uint16_t count = slotCount(state);
  if (count == 0) return true;
  return static_cast<uint32_t>(window) * kSlotsPerWindow < count;
}

uint16_t PresetMode::slotForPad(const UiState& state, uint8_t pad) const {
  const uint32_t slot =
      static_cast<uint32_t>(state.presetWindow) * kSlotsPerWindow + pagedSlot(state.presetPage, pad);
  const uint16_t count = slotCount(state);
  // A pad past the end of the device's list is a voice it hasn't got: dark, and deaf to a tap.
  if (slot >= kNoSlot || (count != 0 && slot >= count)) return kNoSlot;
  return static_cast<uint16_t>(slot);
}

void PresetMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  // Shift + the bottom row picks the window, the same row and modifier pattern mode turns
  // into the mutes. Everything else is a preset, Shift or not.
  if (pressed && state.shiftHeld && pad / kGridCols == kWindowRow) {
    const uint8_t window = static_cast<uint8_t>(pad % kGridCols);
    if (windowHasVoices(state, window)) state.presetWindow = window;
    return;
  }
  SlotGridMode::handlePad(state, pad, pressed);
}

void PresetMode::renderPads(const UiState& state, LedFrame& frame) const {
  SlotGridMode::renderPads(state, frame);
  if (!state.shiftHeld) return;
  // The same colours the pages use, so a window reads like a page: green the one on screen,
  // blue one holding voices, grey one past the end of the list.
  for (uint8_t column = 0; column < kGridCols; ++column) {
    const uint8_t pad = static_cast<uint8_t>(kWindowRow * kGridCols + column);
    frame.pads[pad] = column == state.presetWindow ? kSelectedColor
                      : windowHasVoices(state, column) ? kFilledColor
                                                       : kEmptyColor;
  }
}

const char* PresetMode::padLabel(const UiState& state, uint8_t pad) const {
  if (!state.shiftHeld || pad / kGridCols != kWindowRow) return NULL;
  return kWindowLabels[pad % kGridCols];
}

bool PresetMode::isSelected(const UiState& state, uint16_t slot) const {
  return sequencer_.trackPreset(state.track) == slot;
}

void PresetMode::select(UiState& state, uint16_t slot) {
  sequencer_.setTrackPreset(state.track, slot);
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kTrackColumn = {54, 214, 95};
const Rgb kPatternRow = {30, 156, 255};
const Rgb kMuteColor = {255, 130, 0};
const Rgb kSlotColor = {30, 156, 255};
const Rgb kOpenColor = {54, 214, 95};
const Rgb kWindowColor = {160, 60, 255};

const ModeLegend kPatternLegend = {
    "R2",
    "Pattern",
    {
        {1, 8, kTrackColumn, "columns", "the 8 tracks of this track page"},
        {1, 8, kPatternRow, "rows", "the 8 patterns of this pattern page"},
        {0, 0, kMuteColor, "SHIFT row 8", "the bottom pad row becomes the mutes"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"B1-B4 track pages, B5-B8 pattern pages - no SHIFT", "R6 + pad + pad copy a pattern",
     "SHIFT + R2 scenes", NULL},
    3,
    {kPatternRow, kPatternRow, kPatternRow, kPatternRow,
     kPatternRow, kPatternRow, kPatternRow, kPatternRow},
};

const ModeLegend kProjectLegend = {
    "R1",
    "Project",
    {
        {1, 8, kSlotColor, "the grid", "64 project slots, 8 pages in all"},
        {0, 0, kOpenColor, "green", "the open project - page.pad, so 3.08"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    2,
    {"tap a slot open it, saving the open one first", "hold R1 + B1-B8 page",
     "R5 + slot clear - the file goes to the trash folder", NULL},
    3,
    {kSlotColor, kSlotColor, kSlotColor, kSlotColor,
     kSlotColor, kSlotColor, kSlotColor, kSlotColor},
};

const ModeLegend kPresetLegend = {
    "SHIFT + R4",
    "Preset",
    {
        {1, 8, kSlotColor, "the grid", "64 voices a page, 8 pages a window"},
        {0, 0, kOpenColor, "green", "the voice this track plays"},
        {0, 0, kWindowColor, "SHIFT row 8", "the window: 512 voices each, 8 of them"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"hold R4 or SHIFT + B1-B8 page", "pick the track with B1-B8 first",
     "R5 and R6 do nothing - a patch lives in the synth", NULL},
    3,
    {kSlotColor, kSlotColor, kSlotColor, kSlotColor,
     kSlotColor, kSlotColor, kSlotColor, kSlotColor},
};

}  // namespace

const ModeLegend* PatternMode::legend(const UiState&) const { return &kPatternLegend; }
const ModeLegend* ProjectMode::legend(const UiState&) const { return &kProjectLegend; }
const ModeLegend* PresetMode::legend(const UiState&) const { return &kPresetLegend; }

}  // namespace gx
