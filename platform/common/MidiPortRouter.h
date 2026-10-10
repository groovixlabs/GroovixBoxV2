#pragma once

#include <stdint.h>

#include "comm/MidiOutput.h"
#include "engine/Project.h"

namespace gx {

// Sends each message where that MIDI port goes. One port can be routed somewhere other than
// the MIDI hardware - P8 to a socket server, in the rig this was built for - and every other
// port goes where it always did.
//
// The decision is one comparison on the message's port number, taken per message, so nothing
// upstream learns that a port is special: the sequencer still addresses P1..P8 and the log
// still sees everything.
class MidiPortRouter : public MidiOutput {
 public:
  static const uint8_t kNoRoutedPort = 0xFF;

  explicit MidiPortRouter(MidiOutput& hardware)
      : hardware_(hardware), port_(kNoRoutedPort), to_(NULL) {}

  // Sends `port` to `destination` rather than to the MIDI hardware. A null destination, or a
  // port outside P1..P8, puts everything back on the hardware.
  void route(uint8_t port, MidiOutput* destination) {
    const bool usable = destination != NULL && port < kNumMidiPorts;
    port_ = usable ? port : kNoRoutedPort;
    to_ = usable ? destination : NULL;
  }
  void clearRoute() { route(kNoRoutedPort, NULL); }
  bool isRouted(uint8_t port) const { return to_ != NULL && port == port_; }
  uint8_t routedPort() const { return to_ != NULL ? port_ : kNoRoutedPort; }

  void send(const MidiMessage& message) override {
    if (isRouted(message.port)) {
      to_->send(message);
    } else {
      hardware_.send(message);
    }
  }

 private:
  MidiOutput& hardware_;
  uint8_t port_;
  MidiOutput* to_;
};

}  // namespace gx
