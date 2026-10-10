#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/ApcKey25Surface.h"
#include "common/ApcMiniPanelSurface.h"
#include "common/ApcMiniSurface.h"
#include "common/LaunchpadXSurface.h"
#include "common/MidiMixSurface.h"
#include "common/MidiPortRouter.h"
#include "common/SocketMidiOutput.h"
#include "common/VoiceLibrary.h"
#include "linux/AlsaAnnounce.h"
#include "linux/AlsaMidiInput.h"
#include "linux/AlsaMidiOutput.h"
#include "linux/AlsaMidiPort.h"
#include "ui/ControlSurface.h"
#include "ui/DeviceStatus.h"

namespace gx {

// The gear around the sequencer on a Linux box: the MIDI output ports P1..P8 and the control
// surfaces. It finds them, wires the ports to an interface, and answers the devices page in
// global settings.
//
// A device is bound to a ROLE rather than to a class, and which device plays which role falls
// out of what is plugged in - there is no config key for it, because there is only ever one
// sensible answer:
//
//   grid    the sequencer's pads. A Launchpad X if one is there, else an APC mini mk2
//   panel   the pages, the mode buttons, the arrows, the preset windows and SHIFT. The APC
//           mini when the Launchpad took the grid, else an APC Key 25
//   mixer   an Akai MIDI Mix
//
// The point of the fallback is Shift: the Launchpad X's sixteen edge buttons are exactly
// R1-R8 and B1-B8 with none to spare, so the modifier has to live on the panel - and the APC
// has its own Shift button, free. See ApcMiniPanelSurface.
//
// It is the ControlSurface for the hardware as well. Polling drains the PANEL first, then the
// grid, then the mixer: a modifier and the pad it modifies can arrive in the same frame, and
// a Shift-down that landed after the pad would be a Shift that did nothing. Showing to it
// lights all three. A build with a window of its own puts this behind that window; a box with
// no screen uses it as the whole surface.
class MidiRig : public ControlSurface, public DeviceStatus {
 public:
  // The three jobs a control surface can have. A device is bound to one of these, never to a
  // class: core speaks the same language to whatever is playing the role.
  enum Role { kRoleGrid = 0, kRolePanel, kRoleMixer, kNumRoles };

  // Which device took a role, for the startup line and the devices page.
  enum GridKind { kGridNone = 0, kGridApcMini, kGridLaunchpadX };
  enum PanelKind { kPanelNone = 0, kPanelApcMini, kPanelApcKey25 };

  struct Options {
    bool useSurfaces;          // open the control surfaces
    bool wireOutput;           // wire P1..P8 to an interface at startup
    const char* outputDevice;  // name to wire to, overriding the config; NULL for neither
    Options() : useSurfaces(true), wireOutput(true), outputDevice(NULL) {}
  };

  explicit MidiRig(const Options& options);

  // Opens the MIDI ports. False when the ALSA sequencer would not open at all, which leaves
  // the ports silent but everything else working. The surfaces are NOT opened here: which
  // device plays which role comes from the config file, which is read later, so the caller
  // sets the roles and then calls openSurfaces().
  bool open();
  // Looks for the control surfaces and gives each role to a device. Safe to call again - it
  // only fills roles that are empty - which is how a device plugged in mid-session is picked
  // up. Call after setRoleDevice().
  void openSurfaces();
  // Where the sequencer's notes go. Valid whether or not anything is connected. This is the
  // router, so a port sent to a socket server goes there and the rest go to the hardware.
  MidiOutput& output() { return router_; }
  // What is played into it: a keyboard, if one turned up.
  MidiInput& input() { return in_; }
  bool outputOpen() const { return out_.isOpen(); }
  // What the config file said: the device to wire every port to, and a device port for one
  // port. Set these before wire().
  void setOutputName(const std::string& name) { configName_ = name; }
  // The keyboard to listen to, named in the config; empty finds one by itself.
  void setInputName(const std::string& name) { inputName_ = name; }
  // Pins a role to one device, by the id the config uses - "apcmini", "apckey25", "midimix".
  // Empty, or "auto", leaves the rig to choose by what is plugged in, which is the default.
  // A device that cannot play the role, or an id this build has never heard of, is reported
  // and the role goes back to choosing for itself: a typo in a config file should cost you a
  // warning, not your grid. Set before open().
  void setRoleDevice(uint8_t role, const std::string& id);
  // The id the config pinned a role to, or "" for a role choosing for itself.
  const std::string& roleDevice(uint8_t role) const;
  void setPortSpec(uint8_t port, const std::string& spec);
  // Sends a port to a socket server instead of to MIDI hardware: "192.168.1.50:5000". An
  // empty target puts the port back on its hardware. Set before wire(), which opens it.
  void setPortSocket(uint8_t port, const std::string& target);
  // Where a port's socket points, or "" for a port that is on its MIDI hardware.
  std::string portSocketTarget(uint8_t port) const;
  // The voice lists, kept told which device each port is wired to as the ports are wired.
  void setVoiceLibrary(VoiceLibrary* voices) { voices_ = voices; }
  // What each port is wired to, as ALSA names it; empty for a port reaching nothing.
  const std::string& portDeviceName(uint8_t port) const { return out_.portDeviceName(port); }
  // Wires the ports up, printing what it did. Called once at startup and again on a refresh.
  void wire();

  // Call once a frame, on whatever thread draws: never on the clock thread, since reattaching
  // a surface opens ports and allocates. It costs nothing until ALSA says something has come
  // or gone, and then it reattaches the control surfaces and the keyboard by itself and marks
  // the output ports as worth a refresh.
  void pollDevices();

  const std::string& inputDeviceName() const { return in_.deviceName(); }
  const std::string& gridName() const { return gridPort_.portName(); }
  const std::string& panelName() const { return panelPort_.portName(); }
  const std::string& mixerName() const { return mixPort_.portName(); }
  // Which device ended up with each role, so a build can say so without matching names again.
  uint8_t gridKind() const { return gridKind_; }
  uint8_t panelKind() const { return panelKind_; }

  // The mixer panel's BANK LEFT/RIGHT, which move the track page so its strips and the pads
  // agree. False when there is no panel, or nothing was pressed since the last call.
  bool takeBankStep(int8_t& step);

  // ControlSurface: the panel and the grid together.
  bool pollEvent(ControlEvent& event) override;
  void show(const LedFrame& frame) override;

  // DeviceStatus, for the devices page.
  bool deviceConnected(uint8_t device) const override;
  bool portConnected(uint8_t port) const override;
  bool portsNeedRefresh() const override { return portsStale_; }
  const char* outputName() const override { return out_.deviceName().c_str(); }
  void refreshDevices() override;

 private:
  // The three parts of the role rule, in the order they have to run: the grid decides whether
  // the APC is still free to be the panel.
  void openGrid();
  void openPanel();
  void openMixer();
  // Whether the config leaves this device free to take this role: true when the role was not
  // pinned, or was pinned to this very device.
  bool roleAllows(uint8_t role, const char* id) const;
  // Gives a role up, whichever device was playing it.
  void releaseGrid();
  void releasePanel();

  void wireInput();
  void wireOutputs();
  void wireSocket();
  // Drops a surface whose device has gone, so openSurfaces() can take it up again when it
  // comes back with a new client id.
  void dropDeadSurfaces();
  // A port that has just come back may be holding notes from when it went: its note-offs had
  // nowhere to go. Nothing can be done at the moment it drops, but this cleans up after.
  void silencePorts(const bool* before);
  // What each port reaches, as of this wiring. Everything later is measured against it.
  void recordPorts();
  // Whether a client that turned up is one of ours. Opening a port makes a client, and the
  // sequencer announces it exactly like a synth being plugged in.
  bool ownClient(int client) const;

  Options options_;
  AlsaAnnounce announce_;
  bool portsStale_;              // gear has come or gone since the ports were last wired
  std::vector<int> arrived_;     // clients that turned up at the last poll, kept to save churn
  bool portWas_[kNumMidiPorts];  // which ports reached something at the last wiring
  VoiceLibrary* voices_;         // NULL until the voice lists are read
  AlsaMidiOutput out_;
  AlsaMidiInput in_;
  // A role is a plain ControlSurface*, because core speaks one language to all of them and
  // which class is behind a role is the role rule's business and nobody else's. The OWNER is
  // typed, though: a ControlSurface has a protected, non-virtual destructor on purpose - no
  // interface in this tree is ever deleted through a base pointer - so each device a role can
  // be played by keeps its own unique_ptr, and at most one per role is ever set.
  AlsaMidiPort gridPort_;
  std::unique_ptr<ApcMiniSurface> apcGrid_;
  std::unique_ptr<LaunchpadXSurface> lpxGrid_;
  ControlSurface* grid_;
  uint8_t gridKind_;
  AlsaMidiPort panelPort_;
  std::unique_ptr<ApcMiniPanelSurface> apcPanel_;
  std::unique_ptr<ApcKey25Surface> keyPanel_;
  ControlSurface* panel_;
  uint8_t panelKind_;
  // The mixer stays concrete: BANK LEFT/RIGHT is a MIDI Mix feature that no other device has,
  // and takeBankStep reaches for it directly rather than inventing an interface of one.
  AlsaMidiPort mixPort_;
  std::unique_ptr<MidiMixSurface> mix_;
  std::string configName_;
  std::string inputName_;
  std::string roleDevice_[kNumRoles];  // "" for a role the rig chooses for itself
  std::string portSpecs_[kNumMidiPorts];
  // One port may go to a socket server instead of a cable. The router sits in front of the
  // ALSA output, so output() is the same reference whether or not the socket is open.
  SocketMidiOutput socket_;
  MidiPortRouter router_;
  std::string socketTargets_[kNumMidiPorts];
};

}  // namespace gx
