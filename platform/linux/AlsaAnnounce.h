#pragma once

#include <alsa/asoundlib.h>

#include <vector>

namespace gx {

// Tells us when MIDI gear appears or goes away, without ever scanning for it.
//
// The ALSA sequencer has a port of its own - System:Announce - that emits an event whenever a
// client or port is created or destroyed. Subscribing to it means the expensive work, walking
// every client to see what is there now, only happens when something has actually changed.
// A timer would do that walk over and over behind the playhead's back for nothing.
class AlsaAnnounce {
 public:
  AlsaAnnounce();
  ~AlsaAnnounce();
  AlsaAnnounce(const AlsaAnnounce&) = delete;
  AlsaAnnounce& operator=(const AlsaAnnounce&) = delete;

  // Opens a sequencer client of this name and subscribes it to the announcements. False if
  // the sequencer would not open, which just means changes go unnoticed until a refresh.
  bool open(const char* clientName);
  bool isOpen() const { return seq_ != NULL; }

  // Whether anything has come or gone since the last call, draining whatever has queued up.
  // Cheap: it reads a socket that is nearly always empty.
  //
  // `arrived` is filled with the clients that turned up, because opening a port of our own
  // makes a client too and the sequencer announces it like any other: without the ids there
  // is no telling our own comings and goings from a synth being plugged in.
  bool takeChange(std::vector<int>& arrived);

  // Our own client, so a caller can leave it out of `arrived`. -1 before open().
  int clientId() const { return seq_ ? snd_seq_client_id(seq_) : -1; }

 private:
  snd_seq_t* seq_;
  int port_;
};

}  // namespace gx
