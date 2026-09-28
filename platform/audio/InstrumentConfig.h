#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "audio/AudioEngine.h"
#include "engine/Project.h"

namespace gx {

// What each internal instrument slot I1..I16 is: which LV2 plugin it hosts, and where it
// sits in the mix. Read from a text file, in the same spirit as the control map:
//
//   I1          = http://sfztools.github.io/sfizz   # the plugin this slot hosts
//   I1.gain     = 0.8                               # 0..1, ours
//   I1.pan      = -0.3                              # -1 left .. 1 right, ours
//   I1.sfzfile  = ~/samples/drums.sfz               # the plugin's own parameters
//   I1.volume   = -3
//
// Anything that isn't gain or pan is the plugin's: a control port by its symbol, or one of
// its patch parameters by the last part of its URI. They are applied in the order written,
// and a name the plugin doesn't have is reported when it loads.
//
// Slots with no plugin named play the engine's stand-in tone.
struct InstrumentSlot {
  std::string pluginUri;  // empty: no plugin assigned
  float gain;             // 0..1
  float pan;              // -1..1
  // name, value pairs for the plugin, in the order the file gives them.
  std::vector<std::pair<std::string, std::string> > parameters;
};

class InstrumentConfig {
 public:
  InstrumentConfig();

  // Applies a config file's text. On a bad line it stops, returns false and reports the
  // 1-based line; the lines before it are kept.
  bool load(const std::string& text, uint16_t* errorLine = NULL);
  // Reads the file, then load(). False if it can't be opened (errorLine stays 0).
  bool loadFile(const std::string& path, uint16_t* errorLine = NULL);

  const InstrumentSlot& slot(uint8_t instrument) const;
  // Copies the levels into a running engine.
  void applyTo(AudioEngine& engine) const;
  // How many slots name a plugin, for a one-line summary at start-up.
  uint8_t assignedSlots() const;

 private:
  bool applyLine(const std::string& line);

  InstrumentSlot slots_[kNumInstruments];
  InstrumentSlot none_;
};

}  // namespace gx
