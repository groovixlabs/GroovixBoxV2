#pragma once

#include <alsa/asoundlib.h>

#include <string>
#include <vector>

#include "common/MidiPort.h"

namespace gx {

// MidiPort on the ALSA sequencer. It connects both ways to the first port whose name
// contains a given text, e.g. "APC mini mk2 Control", so other programs can share the device.
class AlsaMidiPort : public MidiPort {
 public:
  AlsaMidiPort();
  ~AlsaMidiPort();
  AlsaMidiPort(const AlsaMidiPort&) = delete;
  AlsaMidiPort& operator=(const AlsaMidiPort&) = delete;

  // Opens a sequencer client with this name and connects it to the matching port. Returns
  // false if the sequencer can't be opened or no such port exists.
  bool open(const char* clientName, const char* portNameContains);
  // Lets go of the sequencer client, so open() can be tried again when a device comes back.
  void close();
  // Whether the device is still on the other end: a port with a live subscription. Unplugging
  // takes the subscription with it, so this goes false on its own.
  bool connected() const;
  const std::string& portName() const { return portName_; }
  // Our own client on the sequencer, so its comings and goings can be told from a device's.
  int clientId() const { return seq_ ? snd_seq_client_id(seq_) : -1; }

  size_t read(uint8_t* buffer, size_t capacity) override;
  bool write(const uint8_t* data, size_t size) override;

 private:
  bool send(snd_seq_event_t& event);

  snd_seq_t* seq_;
  int port_;                    // our port
  snd_midi_event_t* decoder_;   // received events back into bytes
  snd_midi_event_t* encoder_;   // bytes into events to send
  std::vector<uint8_t> received_;  // decoded bytes not yet read
  std::string portName_;
};

}  // namespace gx
