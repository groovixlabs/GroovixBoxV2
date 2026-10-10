#pragma once

#include <string>

#include "app/GroovixApp.h"

namespace gx {

// The one config file a rig has: which CC each fader and knob sends (read by the control map)
// and where the MIDI ports go (read here). Both kinds of line live in the same file, so a rig
// is described in one place.
//
// It is plain text: "name = value", '#' or ';' starts a comment, blank lines are ignored.
class RigConfig {
 public:
  // Reads the file and applies the control lines to the app. A missing file at the default
  // path leaves the built-in assignments in place and is not an error; a required one that
  // will not open is reported. Either way the object is usable afterwards.
  void load(GroovixApp& app, const std::string& path, bool required);

  // The value of a key, or "" when the file didn't have it. Comments and spaces are stripped.
  std::string value(const char* key) const;
  // Which device the ports go out of, and the device port of one MIDI port, 0-based.
  std::string outputName() const { return value("midiout"); }
  std::string portSpec(uint8_t port) const;
  // "host:port" when the config sends this MIDI port to a socket server instead of to MIDI
  // hardware ("p8.socket = 192.168.1.50:5000"), or "" when it does not.
  std::string portSocket(uint8_t port) const;
  // Which ports Start and Stop may go out of, one bit per port, for setTransportPorts().
  // "p3.transport = off" clears port 3's bit and leaves its clock alone, which is what gear
  // with a sequencer of its own needs: the tempo without being told to play. A port with no
  // line keeps its bit, so a rig that never mentions this behaves as it always has.
  uint8_t transportPorts() const;
  // And which ports the clock goes out of, for setClockPorts(). "p3.clock = off" stops the
  // 24-a-beat stream on port 3. Separate from the transport because the two are separate
  // masks in the sink: a port can have either, both or neither, and a device that keeps its
  // own time wants neither while one following our tempo wants the clock at least.
  uint8_t clockPorts() const;
  // Which device plays each role, by the id MidiRig knows it as ("apcmini", "apckey25",
  // "midimix"), or "" when the file leaves the choice to the rig. The rig decides what the
  // ids mean and reports one it does not know - this only hands the words over.
  std::string gridDevice() const { return value("surface.grid"); }
  std::string panelDevice() const { return value("surface.panel"); }
  std::string mixerDevice() const { return value("surface.mixer"); }

 private:
  // The value of "p<n>" or "p<n>.<suffix>", the one place a per-port key is spelled.
  std::string portValue(uint8_t port, const char* suffix) const;
  // An "off"/"on" switch per port as a bitmask, for the two that read that way.
  uint8_t portMask(const char* suffix) const;

  std::string text_;
};

}  // namespace gx
