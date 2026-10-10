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

const uint8_t kMutedGridLevel = 70;  // how far a muted track's grid drops

}  // namespace

// A muted track's whole grid dims. Nothing it plays is heard, so a pattern lit as if it were
// sounding would be a lie, and this is the one cue that says which of the two you are looking
// at while you audition patterns inside a scene.
void PatternMode::renderPads(const UiState& state, LedFrame& frame) const {
  SlotGridMode::renderPads(state, frame);
  if (!sequencer_.trackMuted(state.track)) return;
  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    frame.pads[pad] = dim(frame.pads[pad], kMutedGridLevel);
  }
}

// Pad 0..63 is pattern 1..64 of the selected track, reading the grid left to right and top
// down. A build with fewer patterns than pads leaves the rest of the grid dark.
uint16_t PatternMode::slotForPad(const UiState& state, uint8_t pad) const {
  if (pad >= kNumPatterns) return kNoSlot;
  return static_cast<uint16_t>(state.track * kNumPatterns + pad);
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

void PatternMode::select(UiState&, uint16_t slot) {
  sequencer_.selectPattern(slotTrack(slot), slotPattern(slot));
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

// Window, page and voice, all 1-based as the surface counts them. The three are the address
// of one voice read from the outside in - window of 512, page of 64 within it, then the voice
// itself - and the third is the whole number, not the pad's place on the page, because that
// is what a device's own voice list is numbered by. They are worth spelling out because a
// window and a page are each one pad among eight identical ones, and because the voice stays
// the track's while you page away from it: the green pad goes off screen, this does not.
uint8_t PresetMode::displayValues(const UiState& state, DisplayValue* values) const {
  uint8_t count = 0;
  values[count].key = "WINDOW";
  values[count].text = NULL;
  values[count].number = static_cast<uint16_t>(state.presetWindow + 1);
  values[count].suffix = NULL;
  ++count;
  values[count].key = "PAGE";
  values[count].text = NULL;
  values[count].number = static_cast<uint16_t>(state.presetPage + 1);
  values[count].suffix = NULL;
  ++count;
  values[count].key = "PRESET";
  values[count].text = NULL;
  values[count].number = static_cast<uint16_t>(sequencer_.trackPreset(state.track) + 1);
  values[count].suffix = NULL;
  // The number alone is no use for picking a sound, so ask the screen to name the voice from
  // the device's own list. A track on an internal instrument has no list and no port, so it
  // gets the number and nothing else.
  values[count].voicePort = sequencer_.trackInstrument(state.track) == kNoInstrument
                                ? sequencer_.trackMidiPort(state.track)
                                : kNoDisplayPort;
  ++count;
  return count;
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kTrackColumn = {54, 214, 95};
const Rgb kPatternRow = {30, 156, 255};
const Rgb kSlotColor = {30, 156, 255};
const Rgb kOpenColor = {54, 214, 95};
const Rgb kWindowColor = {160, 60, 255};

const ModeLegend kPatternLegend = {
    "R4",
    "Pattern",
    {
        {1, 4, kPatternRow, "rows 1-4", "patterns 1-32 of the selected track"},
        {5, 8, kFactoryColor, "rows 5-8", "patterns 33-64, the built-in bank"},
        {0, 0, kTrackColumn, "B1-B8", "the track, SHIFT + B1-B8 the track page"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"R5 + pad clear a pattern", "R6 + pad + pad copy a pattern", "SHIFT + R4 scenes", NULL},
    3,
    {kPatternRow, kPatternRow, kPatternRow, kPatternRow,
     kFactoryColor, kFactoryColor, kFactoryColor, kFactoryColor},
};

const ModeLegend kProjectLegend = {
    "SHIFT + R5",
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
    {"tap a slot open it, saving the open one first", "SHIFT + B1-B8 page",
     "R5 + slot clear - the file goes to the trash folder", NULL},
    3,
    {kSlotColor, kSlotColor, kSlotColor, kSlotColor,
     kSlotColor, kSlotColor, kSlotColor, kSlotColor},
};

const ModeLegend kPresetLegend = {
    "SHIFT + R2",
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
    {"hold R2 or SHIFT + B1-B8 page", "pick the track with B1-B8 first",
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
