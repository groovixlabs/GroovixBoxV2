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

 private:
  std::string text_;
};

}  // namespace gx
