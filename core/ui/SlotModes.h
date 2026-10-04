#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Library.h"
#include "ui/Mode.h"
#include "ui/PresetCatalog.h"

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
  // What a slot that holds something lights. Blue unless a mode has a reason to say more -
  // pattern mode uses it to mark the built-in bank, which behaves differently from your own.
  virtual Rgb filledColor(const UiState& state, uint16_t slot) const;
  virtual void clear(UiState& state, uint16_t slot);
  virtual void copy(UiState& state, uint16_t from, uint16_t to);
};

// R1: each pad is a project slot on the page chosen with R1 + B1..B8.
class ProjectMode : public SlotGridMode {
 public:
  explicit ProjectMode(Library& library);

  const ModeLegend* legend(const UiState& state) const override;

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
  const ModeLegend* legend(const UiState& state) const override;

 protected:
  uint16_t slotForPad(const UiState& state, uint8_t pad) const override;
  bool isSelected(const UiState& state, uint16_t slot) const override;
  bool hasData(const UiState& state, uint16_t slot) const override;
  Rgb filledColor(const UiState& state, uint16_t slot) const override;
  void select(UiState& state, uint16_t slot) override;
  void clear(UiState& state, uint16_t slot) override;
  void copy(UiState& state, uint16_t from, uint16_t to) override;

 private:
  Sequencer& sequencer_;
};

// R4: each pad is one voice of the selected track's device, on the page chosen with R4 + B1..B8
// or Shift + B1..B8. Tapping one sends its Bank Select and Program Change; the slots hold
// nothing of ours.
//
// A device can have far more voices than the 8 pages of 64 reach - a PSR-SX920 has 1650 - so
// the pages sit inside a window of 512, and Shift + the bottom pad row picks the window, the
// same row and modifier pattern mode uses for the mutes. Eight windows reach 4096 voices.
// Pads and windows past the end of the device's list go dark and do nothing.
class PresetMode : public SlotGridMode {
 public:
  explicit PresetMode(Sequencer& sequencer);

  // How long each device's voice list is, so the pads that lead nowhere can say so. Unset
  // leaves the whole grid open, which is right for a build with no lists to read.
  void setCatalog(const PresetCatalog* catalog) { catalog_ = catalog; }

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  const char* padLabel(const UiState& state, uint8_t pad) const override;
  const ModeLegend* legend(const UiState& state) const override;

  // Which window and page a slot sits in, for opening the mode on the track's own voice.
  static uint8_t windowOf(uint16_t slot);
  static uint8_t pageInWindow(uint16_t slot);

 protected:
  bool slotsHoldData() const override { return false; }
  uint16_t slotForPad(const UiState& state, uint8_t pad) const override;
  bool isSelected(const UiState& state, uint16_t slot) const override;
  void select(UiState& state, uint16_t slot) override;

 private:
  // How many slots the selected track's device offers, or 0 when nothing has said.
  uint16_t slotCount(const UiState& state) const;
  bool windowHasVoices(const UiState& state, uint8_t window) const;

  Sequencer& sequencer_;
  const PresetCatalog* catalog_;
};

}  // namespace gx
