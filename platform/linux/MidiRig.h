#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/ApcMiniSurface.h"
#include "common/MidiMixSurface.h"
#include "linux/AlsaAnnounce.h"
#include "linux/AlsaMidiInput.h"
#include "linux/AlsaMidiOutput.h"
#include "linux/AlsaMidiPort.h"
#include "ui/ControlSurface.h"
#include "ui/DeviceStatus.h"

namespace gx {

// The gear around the sequencer on a Linux box: the MIDI output ports P1..P8, the pad grid
// (an APC mini mk2) and the mixer panel (an Akai MIDI Mix). It finds them, wires the ports to
// an interface, and answers the devices page in global settings.
//
// It is the ControlSurface for the hardware as well: polling it polls the panel and then the
// grid, and showing to it lights both. A build with a window of its own puts this behind that
// window; a box with no screen uses it as the whole surface.
class MidiRig : public ControlSurface, public DeviceStatus {
 public:
  struct Options {
    bool useSurfaces;          // open the APC and the MIDI Mix
    bool wireOutput;           // wire P1..P8 to an interface at startup
    const char* outputDevice;  // name to wire to, overriding the config; NULL for neither
    Options() : useSurfaces(true), wireOutput(true), outputDevice(NULL) {}
  };

  explicit MidiRig(const Options& options);

  // Opens the MIDI ports and looks for the surfaces. False when the ALSA sequencer would not
  // open at all, which leaves the ports silent but everything else working.
  bool open();
  // Where the sequencer's notes go. Valid whether or not anything is connected.
  MidiOutput& output() { return out_; }
  // What is played into it: a keyboard, if one turned up.
  MidiInput& input() { return in_; }
  bool outputOpen() const { return out_.isOpen(); }
  // What the config file said: the device to wire every port to, and a device port for one
  // port. Set these before wire().
  void setOutputName(const std::string& name) { configName_ = name; }
  // The keyboard to listen to, named in the config; empty finds one by itself.
  void setInputName(const std::string& name) { inputName_ = name; }
  void setPortSpec(uint8_t port, const std::string& spec);
  // Wires the ports up, printing what it did. Called once at startup and again on a refresh.
  void wire();

  // Call once a frame, on whatever thread draws: never on the clock thread, since reattaching
  // a surface opens ports and allocates. It costs nothing until ALSA says something has come
  // or gone, and then it reattaches the control surfaces and the keyboard by itself and marks
  // the output ports as worth a refresh.
  void pollDevices();

  const std::string& inputDeviceName() const { return in_.deviceName(); }
  const std::string& surfaceName() const { return apcPort_.portName(); }
  const std::string& mixerName() const { return mixPort_.portName(); }

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
  void openSurfaces();

  void wireInput();
  void wireOutputs();
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
  AlsaMidiOutput out_;
  AlsaMidiInput in_;
  AlsaMidiPort apcPort_;
  std::unique_ptr<ApcMiniSurface> apc_;
  AlsaMidiPort mixPort_;
  std::unique_ptr<MidiMixSurface> mix_;
  std::string configName_;
  std::string inputName_;
  std::string portSpecs_[kNumMidiPorts];
};

}  // namespace gx
