#pragma once

#include <stdint.h>

namespace gx {

// What a preset slot actually sends: the two halves of Bank Select and the program, ready for
// the wire. `program` is 0-based, the byte itself - voice lists print it from 1, and the
// reader that builds the table is what subtracts the one.
struct VoiceAddress {
  uint8_t msb;
  uint8_t lsb;
  uint8_t program;
};

// The voices one device can play, in the order its maker lists them: preset slot n is the
// nth voice. Every preset goes through this - there is no arithmetic path beside it, so what
// a pad sends is always a row of a table somewhere and can always be named.
//
// It is a view, not storage. The entries live wherever the platform read them from (see
// platform/common/VoiceLibrary), so core neither allocates nor knows about files, and an MCU
// build can point one at a table in flash.
//
// A table with no entries is the built-in list: General MIDI, bank 0, one program per slot.
// That is what a device with nothing said about it plays, and what the tests use.
class VoiceTable {
 public:
  VoiceTable() : entries_(0), count_(0) {}
  VoiceTable(const VoiceAddress* entries, uint16_t count) : entries_(entries), count_(count) {}

  // How many slots this device offers. Preset mode pages over exactly this many.
  uint16_t size() const { return entries_ ? count_ : kGeneralMidiVoices; }

  // The address of a slot. False when the slot is past the end of the list, which is a slot
  // the device has no voice for - nothing is sent for it.
  bool lookup(uint16_t slot, VoiceAddress& address) const;

  // Whether this is the built-in list rather than one read from a file, for reporting.
  bool isBuiltIn() const { return entries_ == 0; }

  static const uint16_t kGeneralMidiVoices = 128;

 private:
  const VoiceAddress* entries_;
  uint16_t count_;
};

// Where the voices for a MIDI port come from. The platform implements it over whatever it
// read at startup, and answers as the ports are wired now - so a port re-patched by a REFRESH
// plays the new device's voices without anything being rebound.
class VoiceSource {
 public:
  virtual ~VoiceSource() {}
  virtual const VoiceTable& voicesFor(uint8_t port) const = 0;
};

}  // namespace gx
