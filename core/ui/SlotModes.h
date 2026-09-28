#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Library.h"
#include "ui/Mode.h"

namespace gx {

// Base for modes where every pad is a slot: tap selects it, Clear + tap clears it,
// Duplicate + tap (source) + tap (destination) copies it, also across pages. Selected
// slots are green, slots with data dim blue, empty slots grey.
// A mode whose slots hold nothing of ours - preset numbers, which name a patch in the gear -
// says so with slotsHoldData(): its pads show only which one is picked, and Clear and
// Duplicate have nothing to act on, so they leave it alone.
class SlotGridMode : public Mode {
 public:
  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;

 protected:
  ~SlotGridMode() {}

  // Slot shown on a pad, including the page offset, or kNoSlot for a pad that shows nothing.
  virtual uint16_t slotForPad(const UiState& state, uint8_t pad) const = 0;
  virtual bool isSelected(const UiState& state, uint16_t slot) const = 0;
  virtual void select(UiState& state, uint16_t slot) = 0;
  // Only for slots that hold something; the rest are never called.
  virtual bool slotsHoldData() const { return true; }
  virtual bool hasData(const UiState& state, uint16_t slot) const;
  virtual void clear(UiState& state, uint16_t slot);
  virtual void copy(UiState& state, uint16_t from, uint16_t to);
};

// R1: each pad is a project slot on the page chosen with R1 + B1..B8.
class ProjectMode : public SlotGridMode {
 public:
  explicit ProjectMode(Library& library);

 protected:
  uint16_t slotForPad(const UiState& state, uint8_t pad) const override;
  bool isSelected(const UiState& state, uint16_t slot) const override;
  bool hasData(const UiState& state, uint16_t slot) const override;
  void select(UiState& state, uint16_t slot) override;
  void clear(UiState& state, uint16_t slot) override;
  void copy(UiState& state, uint16_t from, uint16_t to) override;

 private:
  Library& library_;
};

// R2: columns are the tracks of the track page, rows the patterns of the pattern page
// (top row first). Shift + a pad on the bottom row mutes that column's track, and a muted
// track's whole column dims, so it is plain why it isn't sounding while you try patterns
// against a scene. Tapping a pattern also selects its track: the bottom row pages the grid
// here (B1..B4 the tracks, B5..B8 the patterns) instead of picking tracks.
class PatternMode : public SlotGridMode {
 public:
  explicit PatternMode(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;

 protected:
  uint16_t slotForPad(const UiState& state, uint8_t pad) const override;
  bool isSelected(const UiState& state, uint16_t slot) const override;
  bool hasData(const UiState& state, uint16_t slot) const override;
  void select(UiState& state, uint16_t slot) override;
  void clear(UiState& state, uint16_t slot) override;
  void copy(UiState& state, uint16_t from, uint16_t to) override;

 private:
  Sequencer& sequencer_;
};

// R4: each pad is a preset number for the selected track, on the page chosen with R4 + B1..B8.
// Tapping one sends its Bank Select and Program Change; the slots hold nothing of ours.
class PresetMode : public SlotGridMode {
 public:
  explicit PresetMode(Sequencer& sequencer);

 protected:
  bool slotsHoldData() const override { return false; }
  uint16_t slotForPad(const UiState& state, uint8_t pad) const override;
  bool isSelected(const UiState& state, uint16_t slot) const override;
  void select(UiState& state, uint16_t slot) override;

 private:
  Sequencer& sequencer_;
};

}  // namespace gx
