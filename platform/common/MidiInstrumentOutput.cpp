#include "common/MidiInstrumentOutput.h"

#include <stddef.h>

namespace gx {

static_assert(kNumInstruments <= kNumMidiChannels,
              "instrument In plays on channel n, so there must be a channel for each");

namespace {

// What a preset sends until the platform has read the port's voice list: General MIDI.
const VoiceTable kBuiltInVoices;

}  // namespace

MidiInstrumentOutput::MidiInstrumentOutput(MidiOutput& output, uint8_t port)
    : output_(output), voices_(NULL), port_(port < kNumMidiPorts ? port : kDefaultPort) {}

void MidiInstrumentOutput::setPort(uint8_t port) {
  if (port < kNumMidiPorts) port_ = port;
}

void MidiInstrumentOutput::noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) {
  if (instrument >= kNumInstruments) return;
  output_.send(midiNoteOn(instrument, note, velocity, port_));
}

void MidiInstrumentOutput::noteOff(uint8_t instrument, uint8_t note) {
  if (instrument >= kNumInstruments) return;
  output_.send(midiNoteOff(instrument, note, port_));
}

void MidiInstrumentOutput::controlChange(uint8_t instrument, uint8_t cc, uint8_t value) {
  if (instrument >= kNumInstruments) return;
  const MidiMessage message = {static_cast<uint8_t>(kMidiControlChange | instrument),
                               static_cast<uint8_t>(cc & 0x7F),
                               static_cast<uint8_t>(value & 0x7F), port_};
  output_.send(message);
}

void MidiInstrumentOutput::presetChanged(uint8_t instrument, uint16_t preset) {
  if (instrument >= kNumInstruments) return;

  // The slot is a row of the destination's voice list, which says outright what to send. A
  // slot past the end of the list is a voice the far end hasn't got, and nothing is sent for
  // it rather than something arbitrary being guessed at.
  const VoiceTable& voices = voices_ ? voices_->voicesFor(port_) : kBuiltInVoices;
  VoiceAddress address;
  if (!voices.lookup(preset, address)) return;

  // Both halves of Bank Select every time, then the Program Change that makes the receiver
  // act on them - the same order and the same reasoning as a track on a MIDI port.
  const uint8_t status = static_cast<uint8_t>(kMidiControlChange | instrument);
  const MidiMessage bankMsb = {status, kMidiBankSelectMsb, address.msb, port_};
  const MidiMessage bankLsb = {status, kMidiBankSelectLsb, address.lsb, port_};
  const MidiMessage program = {static_cast<uint8_t>(kMidiProgramChange | instrument),
                               address.program, 0, port_};
  output_.send(bankMsb);
  output_.send(bankLsb);
  output_.send(program);
}

}  // namespace gx
