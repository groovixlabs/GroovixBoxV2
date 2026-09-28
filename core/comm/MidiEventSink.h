#pragma once

#include <stdint.h>

#include "comm/InstrumentOutput.h"
#include "comm/MidiOutput.h"
#include "engine/EventSink.h"
#include "engine/Project.h"

namespace gx {

// Plays engine output on a MIDI output. Each track plays on the MIDI channel and out of the
// MIDI port the engine last announced for it (at first, track n uses channel n % 16 + 1, and
// each page of 8 tracks its own port), and selecting preset slot p sends Bank Select - both
// halves, MSB p / 128 and LSB 0 - followed by Program Change p % 128. The port travels on
// MidiMessage::port, for outputs that drive more than one MIDI destination.
//
// A track moved to an internal instrument stops going to MIDI: it is passed to the
// InstrumentOutput given to setInstruments(), and dropped while there is none.
//
// The engine's clock goes out as MIDI clock, 24 per quarter note, bracketed by Start and
// Stop, on every port by default (see setClockPorts).
class MidiEventSink : public EventSink {
 public:
  explicit MidiEventSink(MidiOutput& output);
  // Where tracks on an internal instrument play. Without one they make no sound.
  void setInstruments(InstrumentOutput* instruments) { instruments_ = instruments; }

  void noteOn(uint8_t track, uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t track, uint8_t note) override;
  void presetChanged(uint8_t track, uint16_t preset) override;
  void trackChannelChanged(uint8_t track, uint8_t channel) override;
  void trackPortChanged(uint8_t track, uint8_t port) override;
  void controlChange(uint8_t track, uint8_t cc, uint8_t value) override;
  void trackInstrumentChanged(uint8_t track, uint8_t instrument) override;
  void clockTick() override;
  void transportStarted() override;
  void transportStopped() override;

  // Which ports the clock and transport go out of, one bit per port, all of them by default.
  // Gear that runs on its own clock ignores them, so this only needs changing to keep a
  // particular port quiet.
  void setClockPorts(uint8_t mask) { clockPorts_ = mask; }
  uint8_t clockPorts() const { return clockPorts_; }

 private:
  void sendRealtime(uint8_t status);

  uint8_t channelFor(uint8_t track) const;
  uint8_t portFor(uint8_t track) const;
  uint8_t instrumentFor(uint8_t track) const;

  MidiOutput& output_;
  InstrumentOutput* instruments_;  // NULL until a synth is attached
  uint8_t channels_[kNumTracks];   // 0..15
  uint8_t ports_[kNumTracks];      // 0..kNumMidiPorts-1
  uint8_t instruments_slots_[kNumTracks];  // 0..kNumInstruments-1, or kNoInstrument
  uint8_t clockPorts_;                     // bit per MIDI port: where the clock goes
};

}  // namespace gx
