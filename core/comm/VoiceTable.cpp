#include "comm/VoiceTable.h"

namespace gx {

bool VoiceTable::lookup(uint16_t slot, VoiceAddress& address) const {
  if (entries_) {
    if (slot >= count_) return false;
    address = entries_[slot];
    return true;
  }
  // The built-in list. General MIDI is bank 0 with one program per slot, which is a table
  // whose every row can be worked out from its index - so it is worked out rather than held.
  if (slot >= kGeneralMidiVoices) return false;
  address.msb = 0;
  address.lsb = 0;
  address.program = static_cast<uint8_t>(slot);
  return true;
}

}  // namespace gx
