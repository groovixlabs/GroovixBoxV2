#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gx {

// A two-way MIDI connection carried as raw bytes, SysEx included: a USB MIDI device, a DIN
// port, a virtual port... Reads and writes never block.
class MidiPort {
 public:
  // Copies up to capacity received bytes into buffer. Returns how many; 0 when none wait.
  virtual size_t read(uint8_t* buffer, size_t capacity) = 0;
  // Sends complete MIDI messages. Returns false if they could not be sent.
  virtual bool write(const uint8_t* data, size_t size) = 0;

 protected:
  ~MidiPort() {}
};

}  // namespace gx
