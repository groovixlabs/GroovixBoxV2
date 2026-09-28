#pragma once

#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/SlotRenderer.h"
#include "audio/SpscQueue.h"

namespace gx {

// The URIDs plugins ask for, mapped to numbers. Plugins map their URIs while they are being
// built, on the main thread, but the contract allows it any time, so this is locked.
class UridMap {
 public:
  UridMap();
  LV2_URID map(const char* uri);
  const char* unmap(LV2_URID urid);

  LV2_URID_Map* mapFeature() { return &map_; }
  LV2_URID_Unmap* unmapFeature() { return &unmap_; }

 private:
  static LV2_URID mapCallback(LV2_URID_Map_Handle handle, const char* uri);
  static const char* unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid);

  std::mutex mutex_;
  std::map<std::string, LV2_URID> ids_;
  std::vector<std::string> uris_;  // index = urid - 1
  LV2_URID_Map map_;
  LV2_URID_Unmap unmap_;
};

// The world of installed LV2 plugins. One per process: scanning is slow, so it happens once
// at start-up, never while audio is running.
class Lv2Host {
 public:
  Lv2Host();
  ~Lv2Host();

  bool begin();  // loads every installed plugin's metadata
  const LilvPlugin* find(const std::string& uri) const;
  LilvWorld* world() const { return world_; }
  UridMap& urids() { return urids_; }

 private:
  LilvWorld* world_;
  const LilvPlugins* plugins_;
  UridMap urids_;
};

// One plugin instance, playing one instrument slot. Built on the main thread; run on the
// audio thread, where it neither allocates nor locks.
class Lv2Plugin : public SlotRenderer {
 public:
  Lv2Plugin();
  ~Lv2Plugin();

  // Instantiates the plugin and connects its ports. Prints why and returns false if the
  // plugin is missing, needs a feature we don't provide, or has no audio output.
  bool load(Lv2Host& host, const std::string& uri, double sampleRate, uint32_t maxBlockFrames);
  void activate();
  void deactivate();

  // SlotRenderer, audio thread only.
  void beginBlock() override;
  void noteOn(uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t note) override;
  void controlChange(uint8_t cc, uint8_t value) override;
  void render(float* left, float* right, uint32_t frames) override;

  // The plugin's settings as LV2 state, for saving with a project, and putting them back.
  // Both talk to the plugin directly, so stop rendering it first (the rack does).
  std::string saveState() const;
  bool restoreState(const std::string& text);
  const std::string& uri() const { return uri_; }

  // Sets one of the plugin's own things by name: a control port by its symbol, or a patch
  // parameter by the last part of its URI ("sfzfile" for sfizz's SFZ file). False if the
  // plugin has nothing by that name, or the value doesn't suit it.
  bool setParameter(const std::string& name, const std::string& value);
  // Names this plugin answers to, for an error message worth reading.
  std::string parameterNames() const;

  const std::string& name() const { return name_; }
  // MIDI messages handed to the plugin, and blocks it has run: a plugin that stays silent
  // usually shows the answer here.
  uint32_t midiSent() const { return midiSent_.load(std::memory_order_relaxed); }
  uint32_t blocksRun() const { return blocksRun_.load(std::memory_order_relaxed); }
  uint32_t audioOutputs() const { return static_cast<uint32_t>(audioOut_.size()); }
  bool usesWorker() const { return workerInterface_ != NULL; }

 private:
  // Work the plugin asks to be done off the audio thread, and its answers coming back.
  struct WorkMessage {
    uint32_t size;
    uint8_t data[512];
  };
  static LV2_Worker_Status scheduleWork(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                        const void* data);
  static LV2_Worker_Status respond(LV2_Worker_Respond_Handle handle, uint32_t size,
                                   const void* data);
  void runWorker();          // the worker thread's loop
  void appendMidi(const uint8_t* bytes, uint32_t size);

  // A patch parameter, waiting to be sent in the next block.
  struct PendingParameter {
    LV2_URID property;
    LV2_URID type;
    float number;
    char text[512];
  };
  // What the plugin declares it can be told.
  struct Parameter {
    std::string name;  // the last part of the property URI
    LV2_URID property;
    LV2_URID range;
  };

  static const void* getPortValue(const char* symbol, void* handle, uint32_t* size,
                                  uint32_t* type);
  static void setPortValue(const char* symbol, void* handle, const void* value, uint32_t size,
                           uint32_t type);

  Lv2Host* host_;
  const LilvPlugin* plugin_;
  LilvInstance* instance_;
  std::string name_;
  std::string uri_;
  // The features and options a plugin was built with, kept because saving and restoring its
  // state needs the same ones.
  LV2_Feature mapFeature_;
  LV2_Feature unmapFeature_;
  LV2_Feature boundedFeature_;
  LV2_Feature optionsFeature_;
  LV2_Feature workerFeature_;
  const LV2_Feature* features_[6];
  LV2_Options_Option options_[4];
  int32_t optionMinBlock_;
  int32_t optionMaxBlock_;
  float optionSampleRate_;
  uint32_t maxBlock_;

  std::vector<uint32_t> audioOut_;      // port indexes, in order
  std::vector<std::vector<float> > outputs_;
  std::vector<float> silence_;          // for any audio input an instrument happens to have
  std::vector<float> controls_;         // one value per port, only control ports are used
  std::vector<uint8_t> atomIn_;         // the MIDI and patch messages we send it
  std::vector<uint8_t> atomOut_;        // whatever it says back, which we ignore for now
  bool hasAtomIn_;

  std::map<std::string, uint32_t> controlPorts_;  // symbol -> port index
  std::vector<Parameter> parameters_;
  SpscQueue<PendingParameter, 16> pending_;

  LV2_Atom_Forge forge_;
  LV2_URID midiEventUrid_;
  LV2_URID atomSequenceUrid_;
  LV2_URID patchSetUrid_;
  LV2_URID patchPropertyUrid_;
  LV2_URID patchValueUrid_;
  LV2_URID pathUrid_;
  LV2_URID stringUrid_;
  LV2_URID floatUrid_;
  LV2_URID intUrid_;
  LV2_Atom_Forge_Frame frame_;

  const LV2_Worker_Interface* workerInterface_;
  LV2_Worker_Schedule schedule_;
  SpscQueue<WorkMessage, 64> requests_;   // audio thread -> worker
  SpscQueue<WorkMessage, 64> responses_;  // worker -> audio thread
  std::thread worker_;
  std::mutex workMutex_;
  std::condition_variable workSignal_;
  std::atomic<bool> workerRunning_;
  std::atomic<uint32_t> droppedWork_;
  std::atomic<uint32_t> midiSent_;
  std::atomic<uint32_t> blocksRun_;
};

}  // namespace gx
