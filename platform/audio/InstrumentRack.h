#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "audio/AudioEngine.h"
#include "audio/InstrumentConfig.h"

namespace gx {

class Lv2Host;
class Lv2Plugin;

// Turns a config into sound: loads the LV2 plugin each slot names and attaches it to the
// engine. Builds and frees plugins on the main thread only.
//
// Without LV2 support compiled in (no lilv at build time), load() reports that and every slot
// keeps the engine's stand-in tone.
class InstrumentRack {
 public:
  InstrumentRack();
  ~InstrumentRack();

  // Returns how many plugins are now playing. Slots whose plugin is missing or won't load are
  // left on the stand-in tone, with the reason printed.
  uint8_t load(const InstrumentConfig& config, AudioEngine& engine, uint32_t sampleRate,
               uint32_t maxBlockFrames);
  // Detaches and frees everything. Stop the audio device first.
  void unload(AudioEngine& engine);

  // ---- Plugin state, for saving with a project ----
  struct SlotState {
    uint8_t slot;
    std::string pluginUri;
    std::string state;  // LV2 state as text
  };

  // Every loaded plugin's settings. The rack stops sounding for a moment while they are
  // asked — once for all of them, not once each.
  std::vector<SlotState> saveAll();
  // Puts them back, skipping any slot that now hosts a different plugin. Returns how many
  // were restored.
  uint8_t restoreAll(const std::vector<SlotState>& states);
  uint8_t pluginCount() const { return static_cast<uint8_t>(slots_.size()); }

  static bool supported();  // was this build made with LV2 hosting?

 private:
  int indexOf(uint8_t slot) const;
  // Takes every plugin off the engine, or puts them all back. Between the two the plugins can
  // be talked to directly, which is not safe while they are rendering.
  void detachAll();
  void attachAll();

  std::unique_ptr<Lv2Host> host_;
  std::vector<std::unique_ptr<Lv2Plugin> > plugins_;
  std::vector<uint8_t> slots_;  // which instrument each plugin plays
  AudioEngine* engine_;         // where they are attached, for pausing one
};

}  // namespace gx
