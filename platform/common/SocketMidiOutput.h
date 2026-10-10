#pragma once

#include <stdint.h>

#include <string>

#include "comm/MidiOutput.h"

namespace gx {

// A MIDI port that goes to a socket server over UDP instead of to a MIDI cable.
//
// What goes out is exactly the bytes a DIN cable would carry - 90 24 64 for a note on, F8 for
// a clock - one datagram per message, no running status and no framing of our own, so
// anything that can read MIDI can read this.
//
// UDP on purpose. send() is called from the sequencer's own timing path, where blocking is
// the one thing that cannot happen: a datagram to a server that is not listening is thrown
// away by the kernel and costs nothing, where a stream would have to connect, notice the
// drop and reconnect, any of which can stall. The cost is that delivery is not guaranteed,
// which on a LAN means the occasional lost message rather than a late one. For a clock that
// repeats 24 times a beat that is the right trade.
//
// It never blocks and never throws: a failed send is counted and forgotten.
class SocketMidiOutput : public MidiOutput {
 public:
  SocketMidiOutput();
  ~SocketMidiOutput();
  SocketMidiOutput(const SocketMidiOutput&) = delete;
  SocketMidiOutput& operator=(const SocketMidiOutput&) = delete;

  // Points it at a server. The host is a dotted IPv4 address; a name is not looked up,
  // because a DNS lookup is exactly the kind of wait this class exists to avoid. Returns
  // false and stays closed on a bad address or a socket the system will not give us.
  bool open(const std::string& host, uint16_t port);
  void close();
  bool isOpen() const { return fd_ >= 0; }

  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }
  // How many messages went out, and how many the kernel would not take.
  uint32_t sent() const { return sent_; }
  uint32_t dropped() const { return dropped_; }

  void send(const MidiMessage& message) override;

  // Splits "192.168.1.50:5000" into its parts. False when it is not that shape, or when the
  // port is not 1..65535. The host is not checked here; open() does that.
  static bool parseTarget(const std::string& text, std::string& host, uint16_t& port);

 private:
  int fd_;
  std::string host_;
  uint16_t port_;
  uint32_t sent_;
  uint32_t dropped_;
  // The resolved destination, kept so send() does no work beyond one sendto().
  unsigned char addr_[128];
  unsigned int addrLen_;
};

}  // namespace gx
