#pragma once

#include <stdint.h>

#include "comm/MidiMessage.h"

namespace gx {

// Bytes off a wire into whole MIDI messages. It keeps the running status a MIDI stream may
// rely on, lets the one-byte realtime messages through from wherever they interrupt, and
// throws away anything it doesn't understand rather than guessing - a keyboard's aftertouch
// and pitch bend go past without disturbing the note that follows.
//
// No heap and no allocation: it is a few bytes of state, so it runs on an MCU too.
class MidiParser {
 public:
  MidiParser() : status_(0), data1_(0), count_(0) {}

  // Feeds one byte. Returns true when that byte completed a message.
  bool feed(uint8_t byte, MidiMessage& message) {
    if (byte >= 0xF8) {  // clock, start, stop and friends: one byte, from anywhere
      message.status = byte;
      message.data1 = 0;
      message.data2 = 0;
      message.port = 0;
      return true;
    }
    if (byte >= 0x80) {  // a new message begins; system common clears the running status
      status_ = byte < 0xF0 ? byte : 0;
      count_ = 0;
      return false;
    }
    if (status_ == 0) return false;  // data with nothing to belong to
    const uint8_t type = status_ & 0xF0;
    const bool twoBytes = type == kMidiProgramChange || type == 0xD0;  // program, aftertouch
    if (count_ == 0 && !twoBytes) {
      data1_ = byte;
      count_ = 1;
      return false;
    }
    message.status = status_;
    message.data1 = twoBytes ? byte : data1_;
    message.data2 = twoBytes ? 0 : byte;
    message.port = 0;
    count_ = 0;  // running status: the next data byte starts another of the same
    return true;
  }

 private:
  uint8_t status_;  // the running status, 0 for none
  uint8_t data1_;
  uint8_t count_;   // data bytes taken so far
};

}  // namespace gx
