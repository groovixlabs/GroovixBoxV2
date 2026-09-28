#pragma once

#include <alsa/asoundlib.h>

#include <string>

#include "comm/MidiInput.h"
#include "comm/MidiParser.h"

namespace gx {

// MIDI coming in on the ALSA sequencer: a keyboard, a pad controller, another sequencer's
// clock. It is one writable port under its own client, so anything can be wired to it by hand
// as well; connect() subscribes it to a device by name.
class AlsaMidiInput : public MidiInput {
 public:
  AlsaMidiInput();
  ~AlsaMidiInput();
  AlsaMidiInput(const AlsaMidiInput&) = delete;
  AlsaMidiInput& operator=(const AlsaMidiInput&) = delete;

  bool open(const char* clientName);
  bool isOpen() const { return seq_ != NULL; }
  // Subscribes to the first source port of the first client whose name contains this text.
  bool connect(const char* clientNameContains);
  // Subscribes to a keyboard or controller: a card-backed client with outputs that is neither
  // the loopback nor a device we drive ourselves, preferring the one with the fewest inputs,
  // since an interface has several and a keyboard has one. Returns false if nothing fits.
  bool connectFirstController();
  const std::string& deviceName() const { return deviceName_; }
  // Our own client on the sequencer, so its comings and goings can be told from a device's.
  int clientId() const { return seq_ ? snd_seq_client_id(seq_) : -1; }
  // Whether anything is still on the other end; unplugging takes the subscription with it.
  bool connected() const;

  // MidiInput: whole messages, decoded from whatever bytes have arrived.
  bool poll(MidiMessage& message) override;

 private:
  bool subscribe(int client, int port, const char* name);

  snd_seq_t* seq_;
  int port_;
  snd_midi_event_t* decoder_;  // events into the bytes the parser reads
  MidiParser parser_;
  uint8_t bytes_[64];  // decoded but not yet parsed
  uint8_t count_;
  uint8_t at_;
  std::string deviceName_;
};

}  // namespace gx
