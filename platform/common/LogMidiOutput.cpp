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
  // The clock is 24 bytes a beat on every port - some 1500 lines a second across eight of
  // them at 120 BPM - and every one says the same thing. Leaving it out is what makes the
  // rest of the log readable. Start and Stop still print: there are two of those a take, and
  // they are the ones worth seeing.
  if (message.status == kMidiClock) return;
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
        rememberBank(message, msb);
        std::fprintf(out_, "MIDI P%d ch%-2d bank %s (CC %2d) %3d\n", port, channel,
                     msb ? "MSB" : "LSB", message.data1, message.data2);
        break;
      }
      std::fprintf(out_, "MIDI P%d ch%-2d control %3d value %3d\n", port, channel, message.data1,
                   message.data2);
      break;
    case kMidiProgramChange: {
      // The Program Change is where the bank is latched, so this is the line that says which
      // sound the gear lands on: the whole address, and - when the voice lists have been read
      // - the preset pad and the voice's own name.
      VoiceAddress address;
      address.msb = bankByte(message, true);
      address.lsb = bankByte(message, false);
      address.program = message.data1;
      std::fprintf(out_, "MIDI P%d ch%-2d program %3d  bank %d:%d%s\n", port, channel,
                   message.data1, address.msb, address.lsb,
                   describeVoice(message, address).c_str());
      break;
    }
    default:
      // The realtime bytes carry no channel, so the hex form reads as a message on channel 9
      // that it is not. Now that the clock no longer buries them, name the two that are left.
      if (message.status == kMidiStart) {
        std::fprintf(out_, "MIDI P%d start\n", port);
      } else if (message.status == kMidiStop) {
        std::fprintf(out_, "MIDI P%d stop\n", port);
      } else {
        std::fprintf(out_, "MIDI P%d %02X %02X %02X\n", port, message.status, message.data1,
                     message.data2);
      }
      break;
  }
}

// The slot a voice sits at is a row of its device's list, not something the three bytes add
// up to, so it can only be had by looking the address back up. Nothing to look it up in, or
// an address the device's list hasn't got, leaves the line with the bank and program alone -
// which is still the whole of what went out.
std::string LogMidiOutput::describeVoice(const MidiMessage& message,
                                         const VoiceAddress& address) {
  if (!voices_) return std::string();
  uint16_t slot = 0;
  std::string name;
  if (!voices_->findVoice(message.port, address, slot, name)) return std::string();
  char out[160];
  std::snprintf(out, sizeof(out), "  preset %u.%02u%s%s",
                static_cast<unsigned>(slot / kPresetsPerPage + 1),
                static_cast<unsigned>(slot % kPresetsPerPage + 1), name.empty() ? "" : "  ",
                name.c_str());
  return out;
}

// A receiver holds the bank it was last sent until a Program Change uses it, so the log has
// to hold it the same way to report what that Program Change means. One per destination,
// because two tracks on different channels or ports select their sounds independently.
uint8_t& LogMidiOutput::bankByte(const MidiMessage& message, bool msb) {
  const uint8_t port = message.port < kNumMidiPorts ? message.port : 0;
  const uint8_t channel = message.status & 0x0F;
  return msb ? bankMsb_[port][channel] : bankLsb_[port][channel];
}

void LogMidiOutput::rememberBank(const MidiMessage& message, bool msb) {
  bankByte(message, msb) = static_cast<uint8_t>(message.data2 & 0x7F);
}

}  // namespace gx
