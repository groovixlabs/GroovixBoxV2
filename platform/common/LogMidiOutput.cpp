#include "common/LogMidiOutput.h"

namespace gx {

namespace {

// Presets are browsed 64 to a page, and the status line and the manual both name a slot
// page.pad counting from 1 - so 3.08 is page 3, pad 8. The log says the same thing, so what
// it prints can be read straight against the pad that was tapped.
const uint16_t kPresetsPerPage = 64;

}  // namespace

void LogMidiOutput::send(const MidiMessage& message) {
  if (!out_) return;
  const int channel = (message.status & 0x0F) + 1;
  const int port = message.port + 1;
  switch (message.status & 0xF0) {
    case kMidiNoteOn:
      std::fprintf(out_, "MIDI P%d ch%-2d note on  %3d vel %3d\n", port, channel, message.data1,
                   message.data2);
      break;
    case kMidiNoteOff:
      std::fprintf(out_, "MIDI P%d ch%-2d note off %3d\n", port, channel, message.data1);
      break;
    case kMidiControlChange:
      // Bank Select is the one CC worth naming: read as "control 0 value 3" it says nothing,
      // and it is half of what the next Program Change will mean.
      if (message.data1 == kMidiBankSelectMsb || message.data1 == kMidiBankSelectLsb) {
        const bool msb = message.data1 == kMidiBankSelectMsb;
        if (msb) rememberBankMsb(message);
        std::fprintf(out_, "MIDI P%d ch%-2d bank %s (CC %2d) %3d\n", port, channel,
                     msb ? "MSB" : "LSB", message.data1, message.data2);
        break;
      }
      std::fprintf(out_, "MIDI P%d ch%-2d control %3d value %3d\n", port, channel, message.data1,
                   message.data2);
      break;
    case kMidiProgramChange: {
      // The Program Change is where the bank is latched, so this is the line that says which
      // sound the gear lands on: the bank it was given, and the preset pad that bank and this
      // program add up to. A bank here is the MSB - 128 presets each, four of them over the
      // 512 slots - which is what the pads and the status line count in.
      const uint8_t bank = bankMsb(message);
      const uint16_t slot = static_cast<uint16_t>(bank * 128 + message.data1);
      std::fprintf(out_, "MIDI P%d ch%-2d program %3d  preset %d.%02d (bank %d, program %d)\n",
                   port, channel, message.data1, slot / kPresetsPerPage + 1,
                   slot % kPresetsPerPage + 1, bank, message.data1);
      break;
    }
    default:
      std::fprintf(out_, "MIDI P%d %02X %02X %02X\n", port, message.status, message.data1,
                   message.data2);
      break;
  }
}

// A receiver holds the bank it was last sent until a Program Change uses it, so the log has
// to hold it the same way to report what that Program Change means. One per destination,
// because two tracks on different channels or ports select their sounds independently.
uint8_t& LogMidiOutput::bankMsbFor(const MidiMessage& message) {
  const uint8_t port = message.port < kNumMidiPorts ? message.port : 0;
  return bankMsb_[port][message.status & 0x0F];
}

void LogMidiOutput::rememberBankMsb(const MidiMessage& message) {
  bankMsbFor(message) = static_cast<uint8_t>(message.data2 & 0x7F);
}

uint8_t LogMidiOutput::bankMsb(const MidiMessage& message) { return bankMsbFor(message); }

}  // namespace gx
