#pragma once

#include <alsa/asoundlib.h>

#include <string>

#include "comm/MidiOutput.h"
#include "engine/Project.h"

namespace gx {

// The sequencer's MIDI output on the ALSA sequencer: one source port per MIDI port P1..P8,
// so a track's port decides which socket its notes leave by. It is the desktop twin of the
// MIDI output a hardware build has, where the ports are the cables of a USB MIDI interface.
//
// The ports exist whether or not anything is connected to them, so they can be wired up by
// hand (aconnect, qjackctl) at any time; connectDevice() connects the first kNumMidiPorts
// inputs of one device in order, which is how a 4x4 interface becomes P1..P4, and
// connectFirstInterface() picks that device itself when nobody has said which to use.
class AlsaMidiOutput : public MidiOutput {
 public:
  AlsaMidiOutput();
  ~AlsaMidiOutput();
  AlsaMidiOutput(const AlsaMidiOutput&) = delete;
  AlsaMidiOutput& operator=(const AlsaMidiOutput&) = delete;

  // Opens a sequencer client of this name with a source port per MIDI port, named P1..P8.
  bool open(const char* clientName);
  bool isOpen() const { return seq_ != NULL; }

  // Connects P1, P2... to the writable ports of the first client whose name contains this
  // text, in the order the device lists them. Returns how many were connected, 0 if no such
  // client exists. The device's name is kept for reporting.
  uint8_t connectDevice(const char* clientNameContains);
  // Connects to the MIDI interface it can find: a card-backed client with inputs that is not
  // the kernel's own Midi Through and not one of our control surfaces, preferring the one with
  // the most inputs so a 4x4 beats a keyboard that enumerates first. Returns how many ports it
  // wired, 0 if nothing suitable is there.
  uint8_t connectFirstInterface();
  // Wires one port to a named device port, so a USB synth can have a port to itself:
  //   "MIDI4x4 Midi Out 2"  that port, named as a listing prints it
  //   "Digitone"            a device: its first input
  //   "MIDI4x4:2"           its second input, counting the inputs it offers
  //   "Digitone:MIDI 1"     the input whose name contains that, where the port's own name
  //                         is not enough to tell two devices apart
  // Everything matches on part of the name, ignoring case. Returns what it connected to for
  // reporting, or an empty string if nothing matched.
  std::string connectPortTo(uint8_t port, const char* spec);
  const std::string& deviceName() const { return deviceName_; }
  // Our own client on the sequencer, so its comings and goings can be told from a device's.
  int clientId() const { return seq_ ? snd_seq_client_id(seq_) : -1; }
  // Whether anything is listening to that port. Asked of the sequencer rather than remembered,
  // so a port wired up by hand with aconnect counts too, and one that went away stops counting.
  bool portConnected(uint8_t port) const;
  // Whether any port reaches anything. A rescan leaves a wired-up output alone, so a patch
  // made by hand is never pulled out from under you.
  bool anyPortConnected() const;

  // MidiOutput: sends on the source port of message.port. A message for a port the build
  // doesn't have is dropped and counted.
  void send(const MidiMessage& message) override;

  // Messages dropped because their port doesn't exist, for reporting at exit.
  uint32_t dropped() const { return dropped_; }

 private:
  uint8_t connectTo(int client, const char* name);
  uint8_t countSinkPorts(int client) const;
  std::string connectHere(uint8_t port, int client, const char* clientName,
                          snd_seq_port_info_t* info);
  snd_seq_t* seq_;
  int ports_[kNumMidiPorts];  // our source ports, -1 when not created
  std::string deviceName_;
  uint8_t connected_;  // bit per port connected by connectDevice
  uint32_t dropped_;
};

}  // namespace gx
