#pragma once

#include <stdint.h>

namespace gx {

// A bank of internal instruments, for tracks routed to one instead of to a MIDI port. The
// synth itself doesn't exist yet: until something implements this and is attached to the
// MidiEventSink, a track on an instrument makes no sound.
class InstrumentOutput {
 public:
  virtual void noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) = 0;
  virtual void noteOff(uint8_t instrument, uint8_t note) = 0;
  virtual void controlChange(uint8_t instrument, uint8_t cc, uint8_t value) = 0;
  // The track picked another sound preset slot.
  virtual void presetChanged(uint8_t instrument, uint16_t preset) = 0;

 protected:
  ~InstrumentOutput() {}  // see EventSink
};

}  // namespace gx
