#pragma once

#include <stdint.h>

#include "ui/Controls.h"

namespace gx {

// What the platform knows about the gear around it: which control surfaces answered, which
// MIDI ports reach something, and how to go and look again. Global settings shows it on the
// DEVICES page, so a stage rig can be checked without leaving the instrument.
//
// Implemented outside ui/ (by the platform), like Library: the UI never learns what a port
// is made of. A build with nothing to report leaves it unset and the page shows everything
// as absent.
class DeviceStatus {
 public:
  enum Device {
    kDeviceSurface = 0,  // the pad grid: an APC mini mk2 on the desktop
    kDeviceMixer,        // the mixer panel: an Akai MIDI Mix
    kDeviceKeyboard,     // something played into it: a MIDI keyboard
    kNumStatusDevices,
  };

  // Whether that surface is here and answering.
  virtual bool deviceConnected(uint8_t device) const = 0;
  // Whether MIDI port P1..P8 is wired to anything. A port with nothing on it still works;
  // its messages just go nowhere.
  virtual bool portConnected(uint8_t port) const = 0;
  // The device the ports were wired to, for a label. Empty when nothing is wired.
  virtual const char* outputName() const = 0;
  // Look again: pick up whatever has been plugged in since, and wire the ports to it. Called
  // from the refresh pad, never on a timer - scanning takes long enough to be heard.
  virtual void refreshDevices() = 0;
  // Whether gear has come or gone that the output ports have not been wired to yet. The
  // surfaces reattach themselves, but rewiring P1..P8 in the middle of a take is the player's
  // call, so the refresh pad brightens to offer it instead of doing it. A platform with no
  // way to notice never asks.
  virtual bool portsNeedRefresh() const { return false; }

 protected:
  ~DeviceStatus() {}  // see EventSink: held by pointer, never owned
};

}  // namespace gx
