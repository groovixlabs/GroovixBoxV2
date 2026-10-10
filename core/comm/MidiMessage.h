#pragma once

#include <stdint.h>

namespace gx {

struct MidiMessage {
  uint8_t status;  // message type in the high nibble, channel 0..15 in the low nibble
  uint8_t data1;
  uint8_t data2;
  uint8_t port;    // which MIDI output it goes out of, 0-based (kNumMidiPorts in Project.h)
};

static const uint8_t kMidiNoteOff = 0x80;
static const uint8_t kMidiNoteOn = 0x90;
static const uint8_t kMidiControlChange = 0xB0;
static const uint8_t kMidiProgramChange = 0xC0;
// Bank Select is a 14-bit pair of controllers, and the receiver latches it on the Program
// Change that follows. Sending one half alone leaves the other standing at whatever it was
// last given, so both always go out together.
static const uint8_t kMidiBankSelectMsb = 0;   // CC 0: the bank's high 7 bits
static const uint8_t kMidiBankSelectLsb = 32;  // CC 32: its low 7 bits
// System realtime, one byte each: the transport and the clock external gear follows.
static const uint8_t kMidiClock = 0xF8;     // 24 per quarter note while playing
static const uint8_t kMidiStart = 0xFA;     // play, from the beginning
static const uint8_t kMidiContinue = 0xFB;  // not sent yet: playback always starts at step 1
static const uint8_t kMidiStop = 0xFC;

inline MidiMessage midiRealtime(uint8_t status, uint8_t port = 0) {
  MidiMessage m = {status, 0, 0, port};
  return m;
}

inline MidiMessage midiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity,
                              uint8_t port = 0) {
  MidiMessage m = {static_cast<uint8_t>(kMidiNoteOn | (channel & 0x0F)),
                   static_cast<uint8_t>(note & 0x7F), static_cast<uint8_t>(velocity & 0x7F),
                   port};
  return m;
}

inline MidiMessage midiNoteOff(uint8_t channel, uint8_t note, uint8_t port = 0) {
  MidiMessage m = {static_cast<uint8_t>(kMidiNoteOff | (channel & 0x0F)),
                   static_cast<uint8_t>(note & 0x7F), 0, port};
  return m;
}

// How many bytes a message is on a wire, 0 for one we never send (SysEx and system common).
// A program change and channel pressure carry one data byte; the realtime bytes carry none.
inline uint8_t midiMessageLength(uint8_t status) {
  if (status >= 0xF8) return 1;  // clock, start, stop and the rest of the realtime bytes
  if (status >= 0xF0) return 0;  // system common: not sent through MidiMessage
  switch (status & 0xF0) {
    case 0xC0:  // program change
    case 0xD0:  // channel pressure
      return 2;
    default:
      return 3;
  }
}

// Lays a message out as the bytes a DIN cable would carry, standalone - no running status,
// since consecutive messages here may be going to different places. Returns how many bytes
// were written, 0 for a message that is not sent, and `out` must have room for three.
inline uint8_t midiMessageBytes(const MidiMessage& message, uint8_t* out) {
  const uint8_t length = midiMessageLength(message.status);
  if (length >= 1) out[0] = message.status;
  if (length >= 2) out[1] = static_cast<uint8_t>(message.data1 & 0x7F);
  if (length >= 3) out[2] = static_cast<uint8_t>(message.data2 & 0x7F);
  return length;
}

}  // namespace gx
