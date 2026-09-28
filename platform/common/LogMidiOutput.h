#pragma once

#include <cstdio>

#include "comm/MidiOutput.h"
#include "engine/Project.h"

namespace gx {

// Prints MIDI messages as text; stands in for a real MIDI backend on desktop.
// Pass NULL to discard messages.
//
// It keeps the last Bank Select sent to each channel of each port, the way the gear on the
// other end does, so a Program Change can be printed with the bank it will be taken in and
// the preset pad that adds up to.
class LogMidiOutput : public MidiOutput {
 public:
  explicit LogMidiOutput(std::FILE* out) : out_(out), bankMsb_() {}

  void send(const MidiMessage& message) override;

 private:
  uint8_t& bankMsbFor(const MidiMessage& message);
  void rememberBankMsb(const MidiMessage& message);
  uint8_t bankMsb(const MidiMessage& message);

  std::FILE* out_;
  uint8_t bankMsb_[kNumMidiPorts][kNumMidiChannels];
};

}  // namespace gx
