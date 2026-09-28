#include "audio/InstrumentRack.h"

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef GX_AUDIO_LV2
#include "audio/Lv2Host.h"
#endif

namespace gx {

InstrumentRack::InstrumentRack() : engine_(NULL) {}

InstrumentRack::~InstrumentRack() {}

bool InstrumentRack::supported() {
#ifdef GX_AUDIO_LV2
  return true;
#else
  return false;
#endif
}

#ifdef GX_AUDIO_LV2

int InstrumentRack::indexOf(uint8_t slot) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i] == slot) return static_cast<int>(i);
  }
  return -1;
}

// Talking to a plugin about its state is not safe while it is rendering, so the whole rack
// comes off the engine for as long as that takes — one pause, not one per slot.
void InstrumentRack::detachAll() {
  if (!engine_ || plugins_.empty()) return;
  for (size_t i = 0; i < slots_.size(); ++i) engine_->setSlotRenderer(slots_[i], NULL);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the block in flight finish
}

void InstrumentRack::attachAll() {
  if (!engine_) return;
  for (size_t i = 0; i < slots_.size(); ++i) {
    engine_->setSlotRenderer(slots_[i], plugins_[i].get());
  }
}

std::vector<InstrumentRack::SlotState> InstrumentRack::saveAll() {
  std::vector<SlotState> states;
  if (plugins_.empty()) return states;
  detachAll();
  for (size_t i = 0; i < plugins_.size(); ++i) {
    SlotState saved;
    saved.slot = slots_[i];
    saved.pluginUri = plugins_[i]->uri();
    saved.state = plugins_[i]->saveState();
    if (!saved.state.empty()) states.push_back(saved);
  }
  attachAll();
  return states;
}

uint8_t InstrumentRack::restoreAll(const std::vector<SlotState>& states) {
  if (plugins_.empty() || states.empty()) return 0;
  detachAll();
  uint8_t restored = 0;
  for (size_t i = 0; i < states.size(); ++i) {
    const int index = indexOf(states[i].slot);
    if (index < 0) continue;
    if (!states[i].pluginUri.empty() && states[i].pluginUri != plugins_[index]->uri()) {
      std::fprintf(stderr, "I%d: the project wants %s, but this slot hosts %s\n",
                   states[i].slot + 1, states[i].pluginUri.c_str(),
                   plugins_[index]->uri().c_str());
      continue;
    }
    if (plugins_[index]->restoreState(states[i].state)) ++restored;
  }
  attachAll();
  return restored;
}

uint8_t InstrumentRack::load(const InstrumentConfig& config, AudioEngine& engine,
                             uint32_t sampleRate, uint32_t maxBlockFrames) {
  engine_ = &engine;
  uint8_t wanted = 0;
  for (uint8_t slot = 0; slot < kNumInstruments; ++slot) {
    if (!config.slot(slot).pluginUri.empty()) ++wanted;
  }
  if (wanted == 0) return 0;

  if (!host_) host_.reset(new Lv2Host());
  if (!host_->begin()) {
    std::fprintf(stderr, "lv2: no plugins could be scanned\n");
    return 0;
  }

  uint8_t loaded = 0;
  for (uint8_t slot = 0; slot < kNumInstruments; ++slot) {
    const std::string& uri = config.slot(slot).pluginUri;
    if (uri.empty()) continue;

    std::unique_ptr<Lv2Plugin> plugin(new Lv2Plugin());
    if (!plugin->load(*host_, uri, sampleRate, maxBlockFrames)) continue;

    // Whatever else the slot named: control ports by symbol, patch parameters by name.
    const std::vector<std::pair<std::string, std::string> >& parameters =
        config.slot(slot).parameters;
    for (size_t i = 0; i < parameters.size(); ++i) {
      if (plugin->setParameter(parameters[i].first, parameters[i].second)) continue;
      std::fprintf(stderr, "I%d: %s has nothing called '%s'. It answers to: %s\n", slot + 1,
                   plugin->name().c_str(), parameters[i].first.c_str(),
                   plugin->parameterNames().c_str());
    }
    plugin->activate();
    std::printf("I%d: %s (%u audio out%s%s)\n", slot + 1, plugin->name().c_str(),
                plugin->audioOutputs(), plugin->audioOutputs() == 1 ? "" : "s",
                plugin->usesWorker() ? ", worker" : "");
    engine.setSlotRenderer(slot, plugin.get());
    slots_.push_back(slot);
    plugins_.push_back(std::move(plugin));
    ++loaded;
  }
  return loaded;
}

void InstrumentRack::unload(AudioEngine& engine) {
  for (size_t i = 0; i < plugins_.size(); ++i) {
    std::printf("I%d: %u MIDI messages, %u blocks run\n", slots_[i] + 1, plugins_[i]->midiSent(),
                plugins_[i]->blocksRun());
  }
  for (size_t i = 0; i < slots_.size(); ++i) engine.setSlotRenderer(slots_[i], NULL);
  slots_.clear();
  plugins_.clear();  // deactivates and frees each one
  host_.reset();
}

#else  // no lilv at build time

int InstrumentRack::indexOf(uint8_t) const { return -1; }
void InstrumentRack::detachAll() {}
void InstrumentRack::attachAll() {}
std::vector<InstrumentRack::SlotState> InstrumentRack::saveAll() {
  return std::vector<SlotState>();
}
uint8_t InstrumentRack::restoreAll(const std::vector<SlotState>&) { return 0; }


uint8_t InstrumentRack::load(const InstrumentConfig& config, AudioEngine&, uint32_t, uint32_t) {
  for (uint8_t slot = 0; slot < kNumInstruments; ++slot) {
    if (config.slot(slot).pluginUri.empty()) continue;
    std::fprintf(stderr, "lv2: this build cannot host plugins (built without lilv)\n");
    break;
  }
  return 0;
}

void InstrumentRack::unload(AudioEngine&) {}

#endif

}  // namespace gx
