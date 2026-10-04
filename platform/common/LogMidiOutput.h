#pragma once

#include <cstdio>

#include <string>

#include "comm/MidiOutput.h"
#include "common/VoiceLibrary.h"
#include "engine/Project.h"

namespace gx {

// Prints MIDI messages as text; stands in for a real MIDI backend on desktop.
// Pass NULL to discard messages.
//
// It keeps the last Bank Select sent to each channel of each port, the way the gear on the
// other end does, so a Program Change can be printed with the bank it lands in. Given the
// voice lists as well, it looks the address up and names the voice - the log sees only what
// goes on the wire, so the name is found from the address rather than passed down to it.
class LogMidiOutput : public MidiOutput {
 public:
  explicit LogMidiOutput(std::FILE* out) : out_(out), voices_(NULL), bankMsb_(), bankLsb_() {}

  // Which voices each port's device has, so a Program Change can be printed with its preset
  // pad and voice name. Without it the line still carries the bank and the program.
  void setVoices(const VoiceLibrary* voices) { voices_ = voices; }

  void send(const MidiMessage& message) override;

 private:
  uint8_t& bankByte(const MidiMessage& message, bool msb);
  void rememberBank(const MidiMessage& message, bool msb);
  std::string describeVoice(const MidiMessage& message, const VoiceAddress& address);

  std::FILE* out_;
  const VoiceLibrary* voices_;  // NULL in a test, or before the lists are read
  uint8_t bankMsb_[kNumMidiPorts][kNumMidiChannels];
  uint8_t bankLsb_[kNumMidiPorts][kNumMidiChannels];
};

}  // namespace gx
