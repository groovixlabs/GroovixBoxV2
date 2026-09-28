#pragma once

#include <stdint.h>

namespace gx {

// Receives what the engine plays: a MIDI output, an internal synth, a test recorder...
class EventSink {
 public:
  virtual void noteOn(uint8_t track, uint8_t note, uint8_t velocity) = 0;
  virtual void noteOff(uint8_t track, uint8_t note) = 0;
  // A track switched to another sound preset slot.
  virtual void presetChanged(uint8_t track, uint16_t preset) = 0;
  // A track now plays on another MIDI channel, 0..15. Sinks that aren't MIDI can ignore it.
  virtual void trackChannelChanged(uint8_t track, uint8_t channel) = 0;
  // A track now plays out of another MIDI port, 0..kNumMidiPorts-1.
  virtual void trackPortChanged(uint8_t track, uint8_t port) = 0;
  // A control moved: send CC number cc with value 0..127 on the track's channel and port.
  virtual void controlChange(uint8_t track, uint8_t cc, uint8_t value) = 0;
  // The track now plays on internal instrument 0..kNumInstruments-1, or on its MIDI port
  // again when instrument is kNoInstrument.
  virtual void trackInstrumentChanged(uint8_t track, uint8_t instrument) = 0;
  // One clock per engine tick while playing: 6 ticks a step and 4 steps a beat make exactly
  // the 24 per quarter note MIDI clock wants. transportStarted() comes just before the first
  // one, transportStopped() when playback stops. Sinks that aren't MIDI can ignore all three.
  virtual void clockTick() = 0;
  virtual void transportStarted() = 0;
  virtual void transportStopped() = 0;

 protected:
  // Interfaces are never deleted through a base pointer. A protected, non-virtual
  // destructor keeps operator delete out of heap-less MCU builds.
  ~EventSink() {}
};

}  // namespace gx
