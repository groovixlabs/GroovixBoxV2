#pragma once

#include "comm/MidiMessage.h"

namespace gx {

// Where MIDI arrives from: a keyboard, a pad controller, another sequencer's clock. The
// platform reads the bytes and hands over whole messages; the app asks for them once a frame.
//
// It is the mirror of MidiOutput, and like it the core never learns what is on the other end.
// A build with nothing plugged in leaves it unset and the sequencer plays only its own pads.
class MidiInput {
 public:
  // Takes the next message, or returns false when there is nothing waiting.
  virtual bool poll(MidiMessage& message) = 0;

 protected:
  ~MidiInput() {}  // see EventSink: held by pointer, never owned
};

}  // namespace gx
