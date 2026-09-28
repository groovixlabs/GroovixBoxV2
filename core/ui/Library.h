#pragma once

#include <stdint.h>

#include "ui/Controls.h"

namespace gx {

// Projects and presets are browsed a page at a time, one pad per slot. Pages are chosen
// with B1..B8 while R1 (projects) or R4 (presets) is held.
static const uint8_t kNumPages = kNumBottomButtons;
static const uint16_t kSlotsPerPage = kNumPads;
static const uint16_t kNumLibrarySlots = kNumPages * kSlotsPerPage;

// Saved projects as the UI sees them. Implemented outside ui/ (by app/), so the UI never
// depends on how or where anything is stored. Slots are numbered across pages:
// slot = page * kSlotsPerPage + pad.
// Presets are not here: a preset is a number the gear resolves, not something we keep, so
// preset mode needs nothing but the Sequencer to set it.
class Library {
 public:
  virtual uint16_t currentProject() const = 0;
  // Whether a slot holds a saved project.
  virtual bool projectHasData(uint16_t slot) const = 0;
  virtual bool projectPageHasData(uint8_t page) const = 0;
  // Saves the current project, then opens the one in slot.
  virtual void selectProject(uint16_t slot) = 0;
  virtual void clearProject(uint16_t slot) = 0;
  virtual void copyProject(uint16_t from, uint16_t to) = 0;

 protected:
  ~Library() {}  // see EventSink
};

}  // namespace gx
