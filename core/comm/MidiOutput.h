#pragma once

#include "comm/MidiMessage.h"

namespace gx {

// A MIDI destination: USB/DIN MIDI on an MCU, CoreMIDI on Mac/iPad, WinMM/ALSA on PC...
class MidiOutput {
 public:
  virtual void send(const MidiMessage& message) = 0;

 protected:
  ~MidiOutput() {}  // see EventSink
};

}  // namespace gx
