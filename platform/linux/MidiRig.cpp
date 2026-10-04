#include "linux/MidiRig.h"

#include <cstdio>

#include "comm/MidiMessage.h"

namespace gx {

MidiRig::MidiRig(const Options& options)
    : options_(options), portsStale_(false), voices_(NULL) {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) portWas_[port] = false;
}

bool MidiRig::open() {
  const bool opened = out_.open("GroovixBox");
  if (!opened) std::fprintf(stderr, "MIDI out: the ALSA sequencer would not open\n");
  if (!in_.open("GroovixBox In")) {
    std::fprintf(stderr, "MIDI in: the ALSA sequencer would not open\n");
  }
  openSurfaces();
  // Without this the gear is still found at startup and by the refresh pad; all that is lost
  // is being told when something changes in between.
  if (!announce_.open("GroovixBox Watch")) {
    std::fprintf(stderr, "devices: not watching for changes; use REFRESH after plugging in\n");
  }
  return opened;
}

// Once a frame, and almost always a no-op: ALSA tells us when there is anything to do.
bool MidiRig::ownClient(int client) const {
  return client < 0 || client == out_.clientId() || client == in_.clientId() ||
         client == apcPort_.clientId() || client == mixPort_.clientId() ||
         client == announce_.clientId();
}

void MidiRig::pollDevices() {
  if (!announce_.takeChange(arrived_)) return;
  bool appeared = false;
  for (size_t i = 0; i < arrived_.size(); ++i) {
    if (!ownClient(arrived_[i])) appeared = true;
  }
  // A surface whose device has gone is dropped whatever happened, but looking for one is only
  // worth it when something turned up: opening a port makes a client of our own, and a client
  // that appears and vanishes again - which is what a failed attempt leaves behind - is
  // indistinguishable afterwards from a synth being plugged in and pulled out.
  dropDeadSurfaces();
  if (appeared) {
    openSurfaces();
    wireInput();
  }
  // The output ports are a different matter: rewiring them mid-take is the player's call, so
  // the refresh pad brightens and waits to be pressed rather than doing it.
  //
  // What is worth offering is a port that no longer reaches what it did - and, when we have
  // no output at all, anything turning up that could be one. A new device while the ports are
  // already wired is somebody else's; saying so every time would make the pad meaningless.
  bool moved = false;
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    if (out_.portConnected(port) != portWas_[port]) moved = true;
  }
  if (moved || (appeared && !out_.anyPortConnected())) portsStale_ = true;
}

void MidiRig::dropDeadSurfaces() {
  if (!options_.useSurfaces) return;
  if (apc_ && !apcPort_.connected()) {
    apc_.reset();
    apcPort_.close();
  }
  if (mix_ && !mixPort_.connected()) {
    mix_.reset();
    mixPort_.close();
  }
}

// All Sound Off then All Notes Off, on every channel of a port that has just come back. A
// note it was holding when it went never got its note-off - there was nowhere to send one.
void MidiRig::silencePorts(const bool* before) {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    if (before[port] || !out_.portConnected(port)) continue;
    for (uint8_t channel = 0; channel < 16; ++channel) {
      const uint8_t status = static_cast<uint8_t>(kMidiControlChange | channel);
      const MidiMessage allSoundOff = {status, 120, 0, port};
      const MidiMessage allNotesOff = {status, 123, 0, port};
      out_.send(allSoundOff);
      out_.send(allNotesOff);
    }
  }
}

void MidiRig::recordPorts() {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    portWas_[port] = out_.portConnected(port);
    // Which voices a port plays follows what is on it. This is the one place every wiring
    // passes through, so a REFRESH that moves a synth to another port moves its voice list
    // with it and nothing has to be rebound by hand.
    if (voices_) {
      voices_->setPortDevice(port, portWas_[port] ? out_.portDeviceName(port) : std::string());
    }
  }
}

// Listens to a keyboard: the one named in the config, else whatever looks like one. Nothing
// is disturbed if it is already subscribed, so a refresh only fills a gap.
void MidiRig::wireInput() {
  if (!in_.isOpen() || in_.connected()) return;
  const bool found = inputName_.empty() ? in_.connectFirstController()
                                        : in_.connect(inputName_.c_str());
  if (found) {
    std::printf("MIDI in: %s\n", in_.deviceName().c_str());
  } else if (!inputName_.empty()) {
    std::fprintf(stderr, "MIDI in: nothing matching '%s'\n", inputName_.c_str());
  }
}

void MidiRig::openSurfaces() {
  if (!options_.useSurfaces) return;
  if (!apc_ && apcPort_.open("GroovixBox Surface", "APC mini mk2 Control")) {
    apc_.reset(new ApcMiniSurface(apcPort_));
    apc_->begin();
    std::printf("control surface: %s\n", apcPort_.portName().c_str());
  }
  if (!mix_ && mixPort_.open("GroovixBox Mixer", "MIDI Mix")) {
    mix_.reset(new MidiMixSurface(mixPort_));
    mix_->clearLeds();
    std::printf("mixer: %s\n", mixPort_.portName().c_str());
  }
}

void MidiRig::setPortSpec(uint8_t port, const std::string& spec) {
  if (port < kNumMidiPorts) portSpecs_[port] = spec;
}

// A port the config gives a device port of its own gets exactly that, which is how a USB synth
// ends up on a port by itself; otherwise P1, P2... go in order to one interface - the one
// named on the command line, else the one named in the config, else the first interface there
// is. A port that already reaches something is left alone, so a patch made by hand with
// aconnect is never pulled out from under you.
void MidiRig::wire() {
  wireOutputs();
  recordPorts();  // every exit from wireOutputs() comes through here
}

void MidiRig::wireOutputs() {
  wireInput();
  if (!out_.isOpen() || !options_.wireOutput) return;
  bool anySpec = false;
  for (uint8_t port = 0; port < kNumMidiPorts && !options_.outputDevice; ++port) {
    if (portSpecs_[port].empty()) continue;
    anySpec = true;
    if (out_.portConnected(port)) continue;
    const std::string where = out_.connectPortTo(port, portSpecs_[port].c_str());
    if (!where.empty()) {
      std::printf("MIDI out: P%u -> %s\n", port + 1, where.c_str());
    } else {
      std::fprintf(stderr, "MIDI out: P%u: nothing matching '%s'\n", port + 1,
                   portSpecs_[port].c_str());
    }
  }
  if (anySpec || out_.anyPortConnected()) return;  // the config said what it wanted
  const char* asked = options_.outputDevice ? options_.outputDevice
                      : configName_.empty() ? NULL : configName_.c_str();
  const uint8_t connected = asked ? out_.connectDevice(asked) : out_.connectFirstInterface();
  if (connected > 0) {
    std::printf("MIDI out: P1-P%u -> %s\n", connected, out_.deviceName().c_str());
  } else if (asked) {
    std::fprintf(stderr, "MIDI out: no device matching '%s'; P1-P%u are open for aconnect\n",
                 asked, static_cast<unsigned>(kNumMidiPorts));
  } else {
    std::printf("MIDI out: no interface found; P1-P%u are open for aconnect\n",
                static_cast<unsigned>(kNumMidiPorts));
  }
}

bool MidiRig::takeBankStep(int8_t& step) { return mix_ && mix_->takeBankStep(step); }

bool MidiRig::pollEvent(ControlEvent& event) {
  if (mix_ && mix_->pollEvent(event)) return true;
  if (apc_ && apc_->pollEvent(event)) return true;
  return false;
}

void MidiRig::show(const LedFrame& frame) {
  if (apc_) apc_->show(frame);
  if (mix_) mix_->show(frame);
}

bool MidiRig::deviceConnected(uint8_t device) const {
  if (device == kDeviceSurface) return apc_ && apcPort_.connected();
  if (device == kDeviceMixer) return mix_ && mixPort_.connected();
  if (device == kDeviceKeyboard) return in_.connected();
  return false;
}

bool MidiRig::portConnected(uint8_t port) const { return out_.portConnected(port); }

// Nothing scans on a timer: a scan walks every client of the sequencer, which is not something
// to do behind the playhead's back. The refresh pad in global settings asks for it instead.
void MidiRig::refreshDevices() {
  bool before[kNumMidiPorts];
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) before[port] = out_.portConnected(port);
  wire();  // which looks for a keyboard as well, and records what the ports reach now
  silencePorts(before);
  portsStale_ = false;
  if (!options_.useSurfaces) return;
  dropDeadSurfaces();
  openSurfaces();
}

}  // namespace gx
