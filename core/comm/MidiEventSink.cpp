#include "comm/MidiEventSink.h"

#include <stddef.h>

namespace gx {

static_assert(kNumMidiPorts <= 8, "one clock bit per MIDI port fits in clockPorts_");

MidiEventSink::MidiEventSink(MidiOutput& output)
    : output_(output), instruments_(NULL), clockPorts_(0xFF) {
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    channels_[t] = defaultMidiChannel(t);
    ports_[t] = defaultMidiPort(t);
    instruments_slots_[t] = kNoInstrument;
  }
}

// Realtime messages belong to no track, so they go out of every port that wants the clock.
void MidiEventSink::sendRealtime(uint8_t status) {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    if (clockPorts_ & (1u << port)) output_.send(midiRealtime(status, port));
  }
}

void MidiEventSink::clockTick() { sendRealtime(kMidiClock); }

void MidiEventSink::transportStarted() { sendRealtime(kMidiStart); }

void MidiEventSink::transportStopped() { sendRealtime(kMidiStop); }

uint8_t MidiEventSink::instrumentFor(uint8_t track) const {
  return track < kNumTracks ? instruments_slots_[track] : kNoInstrument;
}

uint8_t MidiEventSink::channelFor(uint8_t track) const {
  return track < kNumTracks ? channels_[track] : defaultMidiChannel(track);
}

uint8_t MidiEventSink::portFor(uint8_t track) const {
  return track < kNumTracks ? ports_[track] : defaultMidiPort(track);
}

void MidiEventSink::noteOn(uint8_t track, uint8_t note, uint8_t velocity) {
  const uint8_t instrument = instrumentFor(track);
  if (instrument != kNoInstrument) {
    if (instruments_) instruments_->noteOn(instrument, note, velocity);
    return;
  }
  output_.send(midiNoteOn(channelFor(track), note, velocity, portFor(track)));
}

void MidiEventSink::noteOff(uint8_t track, uint8_t note) {
  const uint8_t instrument = instrumentFor(track);
  if (instrument != kNoInstrument) {
    if (instruments_) instruments_->noteOff(instrument, note);
    return;
  }
  output_.send(midiNoteOff(channelFor(track), note, portFor(track)));
}

void MidiEventSink::presetChanged(uint8_t track, uint16_t preset) {
  const uint8_t instrument = instrumentFor(track);
  if (instrument != kNoInstrument) {
    if (instruments_) instruments_->presetChanged(instrument, preset);
    return;
  }
  const uint8_t channel = channelFor(track);
  const uint8_t port = portFor(track);
  const uint8_t status = static_cast<uint8_t>(kMidiControlChange | channel);
  // The slot's high bits are the bank and its low seven the program. Both halves of Bank
  // Select go out every time, the LSB explicitly zero: a synth holds the half it isn't sent,
  // so sending only the MSB would ask for a bank that depends on what it was told before us.
  // The Program Change comes last, because that is what makes the receiver act on the bank.
  const MidiMessage bankMsb = {status, kMidiBankSelectMsb,
                               static_cast<uint8_t>((preset >> 7) & 0x7F), port};
  const MidiMessage bankLsb = {status, kMidiBankSelectLsb, 0, port};
  const MidiMessage program = {static_cast<uint8_t>(kMidiProgramChange | channel),
                               static_cast<uint8_t>(preset & 0x7F), 0, port};
  output_.send(bankMsb);
  output_.send(bankLsb);
  output_.send(program);
}

void MidiEventSink::trackChannelChanged(uint8_t track, uint8_t channel) {
  if (track < kNumTracks && channel < kNumMidiChannels) channels_[track] = channel;
}

void MidiEventSink::controlChange(uint8_t track, uint8_t cc, uint8_t value) {
  const uint8_t instrument = instrumentFor(track);
  if (instrument != kNoInstrument) {
    if (instruments_) instruments_->controlChange(instrument, cc, value);
    return;
  }
  const MidiMessage message = {static_cast<uint8_t>(kMidiControlChange | channelFor(track)),
                               static_cast<uint8_t>(cc & 0x7F),
                               static_cast<uint8_t>(value & 0x7F), portFor(track)};
  output_.send(message);
}

void MidiEventSink::trackInstrumentChanged(uint8_t track, uint8_t instrument) {
  if (track >= kNumTracks) return;
  if (instrument < kNumInstruments || instrument == kNoInstrument) {
    instruments_slots_[track] = instrument;
  }
}

void MidiEventSink::trackPortChanged(uint8_t track, uint8_t port) {
  if (track < kNumTracks && port < kNumMidiPorts) ports_[track] = port;
}

}  // namespace gx
