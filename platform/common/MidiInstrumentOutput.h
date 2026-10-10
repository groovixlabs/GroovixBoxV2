#pragma once

#include <stdint.h>

#include "comm/InstrumentOutput.h"
#include "comm/MidiOutput.h"
#include "comm/VoiceTable.h"
#include "engine/Project.h"

namespace gx {

// The internal instruments, played by something else over MIDI.
//
// A track routed to I1..I16 leaves the engine through InstrumentOutput rather than as MIDI,
// and while nothing is attached there it makes no sound at all - it is not a fallback to the
// track's MIDI port, it is silence. On a build with no audio engine of its own this stands in
// for the synth: the notes go out as MIDI on one port, which is the port pointed at a plugin
// host in the rig this was written for.
//
// Instrument In plays on MIDI channel n. There are exactly as many instruments as channels,
// so the mapping is the identity and needs no table; what channel 3 actually plays is the
// host's business, but it is always I3 that asks.
//
// Presets go through the destination port's voice list, the same path a hardware track's take,
// so the host's plugins are named in the port's voice file like any other device's voices and
// preset mode behaves the same whichever side of the instrument/port fork a track sits on.
class MidiInstrumentOutput : public InstrumentOutput {
 public:
  // P8, the port that can be pointed at a socket server.
  static const uint8_t kDefaultPort = kNumMidiPorts - 1;

  explicit MidiInstrumentOutput(MidiOutput& output, uint8_t port = kDefaultPort);

  // Which MIDI port the instruments play out of. A port outside P1..P8 is ignored.
  void setPort(uint8_t port);
  uint8_t port() const { return port_; }

  // Which voices the destination offers, for presets. Without one, every instrument plays the
  // built-in General MIDI list - which is what a test and a build with no config file see.
  void setVoices(const VoiceSource* voices) { voices_ = voices; }

  void noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t instrument, uint8_t note) override;
  void controlChange(uint8_t instrument, uint8_t cc, uint8_t value) override;
  void presetChanged(uint8_t instrument, uint16_t preset) override;

 private:
  MidiOutput& output_;
  const VoiceSource* voices_;  // NULL until the platform has read the voice lists
  uint8_t port_;               // 0..kNumMidiPorts-1
};

}  // namespace gx
