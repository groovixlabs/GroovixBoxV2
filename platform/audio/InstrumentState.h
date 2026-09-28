#pragma once

#include <stdint.h>

#include <vector>

#include "app/ProjectExtras.h"
#include "audio/InstrumentRack.h"
#include "storage/Storage.h"

namespace gx {

// Keeps the hosted plugins' settings with the project, so a song reopens with its sounds.
//
// The project file itself stays plain data the core can read anywhere; this writes a separate
// blob per project slot ("instruments_007"), holding each slot's plugin URI and its LV2 state.
// Restoring only touches slots that still host the plugin the state came from — swapping a
// slot's plugin while it plays is a job for later.
class InstrumentState : public ProjectExtras {
 public:
  InstrumentState(Storage& storage, InstrumentRack& rack);

  void saveProject(uint16_t slot) override;
  void openProject(uint16_t slot) override;
  void clearProject(uint16_t slot) override;

  // Whether the last save or open actually moved any state, for a line at start-up.
  uint8_t slotsSaved() const { return saved_; }
  uint8_t slotsRestored() const { return restored_; }

 private:
  static void keyFor(uint16_t slot, char* key);

  Storage& storage_;
  InstrumentRack& rack_;
  std::vector<uint8_t> buffer_;
  uint8_t saved_;
  uint8_t restored_;
};

}  // namespace gx
