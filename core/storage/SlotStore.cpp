#include "storage/SlotStore.h"

namespace gx {

SlotStore::SlotStore(Storage& storage, const char* prefix, uint16_t numSlots)
    : storage_(storage),
      prefix_(prefix),
      numSlots_(numSlots < kMaxSlots ? numSlots : static_cast<uint16_t>(kMaxSlots)) {
  for (uint8_t w = 0; w < kNumWords; ++w) occupied_[w] = 0;
}

void SlotStore::keyFor(uint16_t slot, char* key) const {
  size_t n = 0;
  for (const char* p = prefix_; *p && n < kMaxPrefixLength; ++p) key[n++] = *p;
  key[n++] = '_';
  if (slot >= 100) key[n++] = static_cast<char>('0' + slot / 100 % 10);
  key[n++] = static_cast<char>('0' + slot / 10 % 10);
  key[n++] = static_cast<char>('0' + slot % 10);
  key[n] = '\0';
}

void SlotStore::setOccupied(uint16_t slot, bool occupied) {
  const uint64_t bit = static_cast<uint64_t>(1) << (slot % 64);
  if (occupied) {
    occupied_[slot / 64] |= bit;
  } else {
    occupied_[slot / 64] &= ~bit;
  }
}

void SlotStore::scan() {
  char key[kKeySize];
  for (uint16_t slot = 0; slot < numSlots_; ++slot) {
    keyFor(slot, key);
    setOccupied(slot, storage_.exists(key));
  }
}

bool SlotStore::hasData(uint16_t slot) const {
  return slot < numSlots_ && ((occupied_[slot / 64] >> (slot % 64)) & 1u) != 0;
}

bool SlotStore::anyData(uint16_t first, uint16_t count) const {
  for (uint32_t slot = first; slot < static_cast<uint32_t>(first) + count; ++slot) {
    if (hasData(static_cast<uint16_t>(slot))) return true;
  }
  return false;
}

bool SlotStore::read(uint16_t slot, uint8_t* buffer, size_t capacity, size_t& size) {
  if (!hasData(slot)) return false;
  char key[kKeySize];
  keyFor(slot, key);
  return storage_.read(key, buffer, capacity, size);
}

bool SlotStore::write(uint16_t slot, const uint8_t* data, size_t size) {
  if (slot >= numSlots_) return false;
  char key[kKeySize];
  keyFor(slot, key);
  if (!storage_.write(key, data, size)) return false;
  setOccupied(slot, true);
  return true;
}

bool SlotStore::remove(uint16_t slot) {
  if (slot >= numSlots_) return false;
  char key[kKeySize];
  keyFor(slot, key);
  if (!storage_.discard(key)) return false;  // a slot only empties because someone asked
  setOccupied(slot, false);
  return true;
}

bool SlotStore::copy(uint16_t from, uint16_t to, uint8_t* scratch, size_t capacity) {
  if (from >= numSlots_ || to >= numSlots_) return false;
  if (from == to) return true;
  if (!hasData(from)) return remove(to);
  size_t size = 0;
  return read(from, scratch, capacity, size) && write(to, scratch, size);
}

}  // namespace gx
