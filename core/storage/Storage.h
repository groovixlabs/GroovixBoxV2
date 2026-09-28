#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gx {

// Persistent key -> bytes store: files on a PC, Mac or iPad; an SD card or flash on an MCU.
// Keys are short lowercase identifiers ([a-z0-9_]) so every backend can use them as names.
class Storage {
 public:
  // Reads the value stored under key into buffer and sets size to its length. Returns
  // false if the key is missing, unreadable, or larger than capacity.
  virtual bool read(const char* key, uint8_t* buffer, size_t capacity, size_t& size) = 0;
  // Stores data under key, replacing any previous value.
  virtual bool write(const char* key, const uint8_t* data, size_t size) = 0;
  virtual bool exists(const char* key) = 0;
  // Deletes the value under key. Returns true if the key no longer exists, including
  // when it never did.
  virtual bool remove(const char* key) = 0;
  // What a user's Clear does. There is no undo and a clear is one tap, so a backend that can
  // keep the old value aside should: it is only ever called for something a person asked to
  // be rid of, never for housekeeping. One that cannot simply deletes.
  virtual bool discard(const char* key) { return remove(key); }

 protected:
  ~Storage() {}  // see EventSink
};

}  // namespace gx
