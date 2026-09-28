#pragma once

#include <stddef.h>
#include <stdint.h>

#include "storage/Storage.h"

namespace gx {

// A numbered set of values in a Storage with an in-memory index of which slots hold data,
// so the UI can show occupancy every frame without touching the storage medium.
// Keys are "<prefix>_<slot>", the slot in decimal with at least two digits:
// "project_07", "project_100".
class SlotStore {
 public:
  enum { kMaxSlots = 512 };

  // prefix must outlive the store; only its first 12 characters are used.
  SlotStore(Storage& storage, const char* prefix, uint16_t numSlots);

  // Rebuilds the occupancy index by checking every slot in storage.
  void scan();

  uint16_t numSlots() const { return numSlots_; }
  bool hasData(uint16_t slot) const;
  // Whether any of the count slots starting at first holds data.
  bool anyData(uint16_t first, uint16_t count) const;

  bool read(uint16_t slot, uint8_t* buffer, size_t capacity, size_t& size);
  bool write(uint16_t slot, const uint8_t* data, size_t size);
  bool remove(uint16_t slot);
  // Copies a slot's value into another slot, using scratch as the transfer buffer.
  // Copying an empty slot empties the destination.
  bool copy(uint16_t from, uint16_t to, uint8_t* scratch, size_t capacity);

 private:
  enum {
    kMaxPrefixLength = 12,
    kKeySize = kMaxPrefixLength + 5,  // prefix, '_', up to three digits, NUL
    kNumWords = (kMaxSlots + 63) / 64,
  };

  void keyFor(uint16_t slot, char* key) const;
  void setOccupied(uint16_t slot, bool occupied);

  Storage& storage_;
  const char* prefix_;
  uint64_t occupied_[kNumWords];  // bit n of word w = slot w * 64 + n holds data
  uint16_t numSlots_;
};

}  // namespace gx
