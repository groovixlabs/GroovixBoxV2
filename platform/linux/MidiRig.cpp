#include "linux/MidiRig.h"

#include <strings.h>

#include <cstdio>

#include "comm/MidiMessage.h"

namespace gx {

namespace {

// Every surface this build can drive: the id the config calls it, the ALSA name it answers
// to, and the roles it is able to play. One table, so a config file, the role rule and the
// startup line cannot disagree about what exists or what it can do.
//
// The ALSA name is matched as a substring, so it is as much of the name as tells one device
// from another - the APC mini's "Control" port matters, because the device has two.
struct SurfaceDevice {
  const char* id;
  const char* alsaName;
  bool canGrid;
  bool canPanel;
  bool canMixer;
};

const SurfaceDevice kDevices[] = {
    //  id            ALSA name                grid   panel  mixer
    // The Launchpad X offers two ports and only the MIDI one carries its pads and buttons -
    // the DAW port stayed silent through the whole of the capture that mapped the device.
    {"launchpadx", "LPX MIDI", true, false, false},
    {"apcmini", "APC mini mk2 Control", true, true, false},
    {"apckey25", "APC Key 25", false, true, false},
    {"midimix", "MIDI Mix", false, false, true},
};
const size_t kNumDevices = sizeof(kDevices) / sizeof(kDevices[0]);

// What each role tries, best first. A role with no device named in the config walks its list
// and takes the first that is plugged in; a role pinned to a device only ever opens that one.
//
// The Launchpad X leads the grid: it is the better instrument there - eight by eight of real
// RGB and velocity that means something - and taking it frees the APC to be the panel, which
// is where SHIFT has to live, because the Launchpad's sixteen edge buttons are exactly
// R1..R8 and B1..B8 with none left over.
const char* const kGridOrder[] = {"launchpadx", "apcmini"};
const char* const kPanelOrder[] = {"apcmini", "apckey25"};
const char* const kMixerOrder[] = {"midimix"};

const SurfaceDevice* deviceById(const char* id) {
  for (size_t i = 0; i < kNumDevices; ++i) {
    if (strcasecmp(kDevices[i].id, id) == 0) return &kDevices[i];
  }
  return NULL;
}

bool devicePlays(const SurfaceDevice& device, uint8_t role) {
  if (role == MidiRig::kRoleGrid) return device.canGrid;
  if (role == MidiRig::kRolePanel) return device.canPanel;
  return device.canMixer;
}

const char* roleName(uint8_t role) {
  if (role == MidiRig::kRoleGrid) return "grid";
  if (role == MidiRig::kRolePanel) return "page panel";
  return "mixer";
}

}  // namespace

MidiRig::MidiRig(const Options& options)
    : options_(options),
      portsStale_(false),
      voices_(NULL),
      grid_(NULL),
      gridKind_(kGridNone),
      panel_(NULL),
      panelKind_(kPanelNone),
      router_(out_) {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) portWas_[port] = false;
}

bool MidiRig::open() {
  const bool opened = out_.open("GroovixBox");
  if (!opened) std::fprintf(stderr, "MIDI out: the ALSA sequencer would not open\n");
  if (!in_.open("GroovixBox In")) {
    std::fprintf(stderr, "MIDI in: the ALSA sequencer would not open\n");
  }
  // The surfaces are not opened here: the config says which device plays which role, and it
  // has not been read yet. The caller sets the roles and calls openSurfaces().
  //
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
         client == gridPort_.clientId() || client == mixPort_.clientId() ||
         client == panelPort_.clientId() || client == announce_.clientId();
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

// A role whose device has gone is given up, so openSurfaces can fill it again when something
// comes back with a new client id. Clearing the kind matters as much as clearing the pointer:
// it is what lets an APC that was the grid become the panel when a better grid turns up.
void MidiRig::dropDeadSurfaces() {
  if (!options_.useSurfaces) return;
  if (grid_ && !gridPort_.connected()) releaseGrid();
  if (panel_ && !panelPort_.connected()) releasePanel();
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

// A role the config pinned to a device only ever opens that device; a role left alone takes
// the first of its list that is plugged in.
bool MidiRig::roleAllows(uint8_t role, const char* id) const {
  const std::string& chosen = roleDevice_[role];
  if (chosen.empty()) return true;
  return strcasecmp(chosen.c_str(), id) == 0;
}

const std::string& MidiRig::roleDevice(uint8_t role) const {
  static const std::string none;
  return role < kNumRoles ? roleDevice_[role] : none;
}

// A config file says which device plays which role. Anything it asks for that this build
// cannot do is reported and dropped, leaving the role to choose for itself: a typo should
// cost a warning, not a grid.
void MidiRig::setRoleDevice(uint8_t role, const std::string& id) {
  if (role >= kNumRoles) return;
  roleDevice_[role].clear();
  if (id.empty() || strcasecmp(id.c_str(), "auto") == 0) return;
  const SurfaceDevice* device = deviceById(id.c_str());
  if (!device) {
    std::fprintf(stderr, "surfaces: no device called '%s'; the %s will choose for itself\n",
                 id.c_str(), roleName(role));
    return;
  }
  if (!devicePlays(*device, role)) {
    std::fprintf(stderr, "surfaces: a %s cannot be the %s; it will choose for itself\n",
                 device->id, roleName(role));
    return;
  }
  roleDevice_[role] = device->id;
}

// Two passes, and the order is the whole point. A role the config PINNED to a device claims
// it before a role that is only choosing for itself gets a look: "the APC is my panel" has to
// beat the grid's habit of taking the APC, or the line would be a lie. Within a pass the grid
// goes first, because what it takes decides what is left for the panel.
void MidiRig::openSurfaces() {
  if (!options_.useSurfaces) return;
  for (uint8_t pass = 0; pass < 2; ++pass) {
    const bool pinned = (pass == 0);
    if (pinned == !roleDevice_[kRoleGrid].empty()) openGrid();
    if (pinned == !roleDevice_[kRolePanel].empty()) openPanel();
    if (pinned == !roleDevice_[kRoleMixer].empty()) openMixer();
  }
}

// The grid is the first device here that is plugged in, and trying them in order IS the
// precedence: opening a port is how we find out the device is there.
void MidiRig::openGrid() {
  if (grid_) return;
  for (size_t i = 0; i < sizeof(kGridOrder) / sizeof(kGridOrder[0]); ++i) {
    const char* id = kGridOrder[i];
    if (!roleAllows(kRoleGrid, id)) continue;
    // One device, one role - the same rule openPanel keeps, from the other side. With the
    // two passes above, this is what lets a pinned panel keep the APC away from an
    // auto-chosen grid.
    if (panelKind_ == kPanelApcMini && strcasecmp(id, "apcmini") == 0) continue;
    const SurfaceDevice* device = deviceById(id);
    if (!gridPort_.open("GroovixBox Surface", device->alsaName)) continue;
    if (strcasecmp(id, "launchpadx") == 0) {
      lpxGrid_.reset(new LaunchpadXSurface(gridPort_));
      lpxGrid_->begin();
      grid_ = lpxGrid_.get();
      gridKind_ = kGridLaunchpadX;
      std::printf("grid: %s (Launchpad X, in Programmer mode)\n",
                  gridPort_.portName().c_str());
    } else {
      apcGrid_.reset(new ApcMiniSurface(gridPort_));
      apcGrid_->begin();
      grid_ = apcGrid_.get();
      gridKind_ = kGridApcMini;
      std::printf("grid: %s\n", gridPort_.portName().c_str());
    }
    return;
  }
}

void MidiRig::releaseGrid() {
  grid_ = NULL;
  gridKind_ = kGridNone;
  apcGrid_.reset();
  lpxGrid_.reset();
  gridPort_.close();
}

void MidiRig::releasePanel() {
  panel_ = NULL;
  panelKind_ = kPanelNone;
  apcPanel_.reset();
  keyPanel_.reset();
  panelPort_.close();
}

// The panel is whatever can be one and is not already the grid. An APC mini is preferred over
// a Key 25 because it carries SHIFT and has pads and buttons to spare, where the Key 25 has
// neither - and with the APC on the grid there is nothing to choose anyway.
void MidiRig::openPanel() {
  if (panel_) return;
  for (size_t i = 0; i < sizeof(kPanelOrder) / sizeof(kPanelOrder[0]); ++i) {
    const char* id = kPanelOrder[i];
    if (!roleAllows(kRolePanel, id)) continue;
    // One device, one role: an APC already playing the grid is not also the panel. This is
    // the rule the whole arrangement turns on - the APC is only free to be the panel once
    // something else has taken the grid.
    if (gridKind_ == kGridApcMini && strcasecmp(id, "apcmini") == 0) continue;
    const SurfaceDevice* device = deviceById(id);
    if (!panelPort_.open("GroovixBox Pages", device->alsaName)) continue;
    if (strcasecmp(id, "apcmini") == 0) {
      apcPanel_.reset(new ApcMiniPanelSurface(panelPort_));
      apcPanel_->begin();
      apcPanel_->clearLeds();
      panel_ = apcPanel_.get();
      panelKind_ = kPanelApcMini;
      std::printf("page panel: %s (APC mini, with SHIFT)\n", panelPort_.portName().c_str());
    } else {
      keyPanel_.reset(new ApcKey25Surface(panelPort_));
      panel_ = keyPanel_.get();
      panelKind_ = kPanelApcKey25;
      std::printf("page panel: %s\n", panelPort_.portName().c_str());
    }
    return;
  }
}

void MidiRig::openMixer() {
  if (mix_) return;
  for (size_t i = 0; i < sizeof(kMixerOrder) / sizeof(kMixerOrder[0]); ++i) {
    const char* id = kMixerOrder[i];
    if (!roleAllows(kRoleMixer, id)) continue;
    const SurfaceDevice* device = deviceById(id);
    if (!mixPort_.open("GroovixBox Mixer", device->alsaName)) continue;
    mix_.reset(new MidiMixSurface(mixPort_));
    mix_->clearLeds();
    std::printf("mixer: %s\n", mixPort_.portName().c_str());
    return;
  }
}

void MidiRig::setPortSpec(uint8_t port, const std::string& spec) {
  if (port < kNumMidiPorts) portSpecs_[port] = spec;
}

void MidiRig::setPortSocket(uint8_t port, const std::string& target) {
  if (port < kNumMidiPorts) socketTargets_[port] = target;
}

std::string MidiRig::portSocketTarget(uint8_t port) const {
  if (port >= kNumMidiPorts || !socket_.isOpen() || router_.routedPort() != port) return "";
  char text[64];
  std::snprintf(text, sizeof(text), "%s:%u", socket_.host().c_str(),
                static_cast<unsigned>(socket_.port()));
  return text;
}

// A port the config points at a socket server leaves by UDP instead of by a cable. Only one
// port can, which is all the rig asks for; the first that names a target wins, and a target
// that will not open leaves that port on its hardware rather than going silent.
void MidiRig::wireSocket() {
  router_.clearRoute();
  socket_.close();
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    if (socketTargets_[port].empty()) continue;
    std::string host;
    uint16_t number = 0;
    if (!SocketMidiOutput::parseTarget(socketTargets_[port], host, number)) {
      std::fprintf(stderr, "MIDI out: P%u: '%s' is not host:port\n", port + 1,
                   socketTargets_[port].c_str());
      continue;
    }
    if (!socket_.open(host, number)) {
      std::fprintf(stderr, "MIDI out: P%u: cannot send to %s - staying on MIDI\n", port + 1,
                   socketTargets_[port].c_str());
      continue;
    }
    router_.route(port, &socket_);
    std::printf("MIDI out: P%u -> %s:%u (UDP)\n", port + 1, host.c_str(),
                static_cast<unsigned>(number));
    return;
  }
}

// A port the config gives a device port of its own gets exactly that, which is how a USB synth
// ends up on a port by itself; otherwise P1, P2... go in order to one interface - the one
// named on the command line, else the one named in the config, else the first interface there
// is. A port that already reaches something is left alone, so a patch made by hand with
// aconnect is never pulled out from under you.
void MidiRig::wire() {
  wireSocket();  // first, so the hardware wiring knows which port has already left by UDP
  wireOutputs();
  recordPorts();  // every exit from wireOutputs() comes through here
}

void MidiRig::wireOutputs() {
  wireInput();
  if (!out_.isOpen() || !options_.wireOutput) return;
  bool anySpec = false;
  for (uint8_t port = 0; port < kNumMidiPorts && !options_.outputDevice; ++port) {
    if (router_.isRouted(port)) continue;  // this one leaves by UDP, not by a cable
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

// The PANEL is drained first, and that order is load-bearing. The panel carries SHIFT, and a
// modifier and the pad it modifies arrive in the same frame as often as not; taking the pad
// first would hand core a plain pad press and then a Shift that modified nothing. Core reads
// state_.shiftHeld at the moment a pad is handled, so the modifier has to be in before it.
bool MidiRig::pollEvent(ControlEvent& event) {
  if (panel_ && panel_->pollEvent(event)) return true;
  if (grid_ && grid_->pollEvent(event)) return true;
  if (mix_ && mix_->pollEvent(event)) return true;
  return false;
}

void MidiRig::show(const LedFrame& frame) {
  if (grid_) grid_->show(frame);
  if (panel_) panel_->show(frame);
  if (mix_) mix_->show(frame);
}

bool MidiRig::deviceConnected(uint8_t device) const {
  if (device == kDeviceSurface) return grid_ && gridPort_.connected();
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
