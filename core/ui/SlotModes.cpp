#include "ui/SlotModes.h"

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
      frame.pads[pad] = kFilledColor;
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

PresetMode::PresetMode(Sequencer& sequencer) : sequencer_(sequencer) {}

uint16_t PresetMode::slotForPad(const UiState& state, uint8_t pad) const {
  return pagedSlot(state.presetPage, pad);
}

bool PresetMode::isSelected(const UiState& state, uint16_t slot) const {
  return sequencer_.trackPreset(state.track) == slot;
}

void PresetMode::select(UiState& state, uint16_t slot) {
  sequencer_.setTrackPreset(state.track, slot);
}

}  // namespace gx
