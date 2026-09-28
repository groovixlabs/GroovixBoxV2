#include "audio/Lv2Host.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/patch/patch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gx {

namespace {

const uint32_t kAtomBufferBytes = 8192;  // plenty for a block's worth of notes and CCs

// "http://sfztools.github.io/sfizz:sfzfile" -> "sfzfile", which is what a config file names.
std::string lastUriPart(const std::string& uri) {
  const size_t at = uri.find_last_of(":#/");
  return at == std::string::npos ? uri : uri.substr(at + 1);
}

}  // namespace

// ---- URID map ----

UridMap::UridMap() {
  map_.handle = this;
  map_.map = mapCallback;
  unmap_.handle = this;
  unmap_.unmap = unmapCallback;
}

LV2_URID UridMap::map(const char* uri) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<std::string, LV2_URID>::iterator found = ids_.find(uri);
  if (found != ids_.end()) return found->second;
  uris_.push_back(uri);
  const LV2_URID urid = static_cast<LV2_URID>(uris_.size());  // 0 means "not mapped"
  ids_[uri] = urid;
  return urid;
}

const char* UridMap::unmap(LV2_URID urid) {
  std::lock_guard<std::mutex> lock(mutex_);
  return urid >= 1 && urid <= uris_.size() ? uris_[urid - 1].c_str() : NULL;
}

LV2_URID UridMap::mapCallback(LV2_URID_Map_Handle handle, const char* uri) {
  return static_cast<UridMap*>(handle)->map(uri);
}

const char* UridMap::unmapCallback(LV2_URID_Unmap_Handle handle, LV2_URID urid) {
  return static_cast<UridMap*>(handle)->unmap(urid);
}

// ---- The world of installed plugins ----

Lv2Host::Lv2Host() : world_(NULL), plugins_(NULL) {}

Lv2Host::~Lv2Host() {
  if (world_) lilv_world_free(world_);
}

bool Lv2Host::begin() {
  if (world_) return true;
  world_ = lilv_world_new();
  if (!world_) return false;
  lilv_world_load_all(world_);  // slow: start-up only, never with audio running
  plugins_ = lilv_world_get_all_plugins(world_);
  return plugins_ != NULL;
}

const LilvPlugin* Lv2Host::find(const std::string& uri) const {
  if (!plugins_) return NULL;
  LilvNode* node = lilv_new_uri(world_, uri.c_str());
  if (!node) return NULL;
  const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins_, node);
  lilv_node_free(node);
  return plugin;
}

// ---- One plugin instance ----

Lv2Plugin::Lv2Plugin()
    : host_(NULL),
      plugin_(NULL),
      instance_(NULL),
      optionMinBlock_(1),
      optionMaxBlock_(0),
      optionSampleRate_(48000.0f),
      maxBlock_(0),
      hasAtomIn_(false),
      midiEventUrid_(0),
      atomSequenceUrid_(0),
      patchSetUrid_(0),
      patchPropertyUrid_(0),
      patchValueUrid_(0),
      pathUrid_(0),
      stringUrid_(0),
      floatUrid_(0),
      intUrid_(0),
      workerInterface_(NULL),
      workerRunning_(false),
      droppedWork_(0),
      midiSent_(0),
      blocksRun_(0) {
  schedule_.handle = this;
  schedule_.schedule_work = scheduleWork;
}

Lv2Plugin::~Lv2Plugin() {
  deactivate();
  if (workerRunning_.exchange(false)) {
    workSignal_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  if (instance_) lilv_instance_free(instance_);
}

bool Lv2Plugin::load(Lv2Host& host, const std::string& uri, double sampleRate,
                     uint32_t maxBlockFrames) {
  const LilvPlugin* plugin = host.find(uri);
  if (!plugin) {
    std::fprintf(stderr, "lv2: no plugin with URI %s is installed\n", uri.c_str());
    return false;
  }
  host_ = &host;
  maxBlock_ = maxBlockFrames;
  LilvNode* nameNode = lilv_plugin_get_name(plugin);
  name_ = nameNode ? lilv_node_as_string(nameNode) : uri;
  if (nameNode) lilv_node_free(nameNode);

  UridMap& urids = host.urids();
  midiEventUrid_ = urids.map(LV2_MIDI__MidiEvent);
  atomSequenceUrid_ = urids.map(LV2_ATOM__Sequence);
  patchSetUrid_ = urids.map(LV2_PATCH__Set);
  patchPropertyUrid_ = urids.map(LV2_PATCH__property);
  patchValueUrid_ = urids.map(LV2_PATCH__value);
  pathUrid_ = urids.map(LV2_ATOM__Path);
  stringUrid_ = urids.map(LV2_ATOM__String);
  floatUrid_ = urids.map(LV2_ATOM__Float);
  intUrid_ = urids.map(LV2_ATOM__Int);

  // Everything a plugin may ask of us. sfizz, for one, refuses to load without the worker.
  const LV2_URID minBlockUrid = urids.map(LV2_BUF_SIZE__minBlockLength);
  const LV2_URID maxBlockUrid = urids.map(LV2_BUF_SIZE__maxBlockLength);
  const LV2_URID sampleRateUrid = urids.map(LV2_PARAMETERS__sampleRate);
  const LV2_URID intUrid = urids.map(LV2_ATOM__Int);
  const LV2_URID floatUrid = urids.map(LV2_ATOM__Float);
  optionMinBlock_ = 1;
  optionMaxBlock_ = static_cast<int32_t>(maxBlockFrames);
  optionSampleRate_ = static_cast<float>(sampleRate);
  const LV2_Options_Option options[4] = {
      {LV2_OPTIONS_INSTANCE, 0, minBlockUrid, sizeof(int32_t), intUrid, &optionMinBlock_},
      {LV2_OPTIONS_INSTANCE, 0, maxBlockUrid, sizeof(int32_t), intUrid, &optionMaxBlock_},
      {LV2_OPTIONS_INSTANCE, 0, sampleRateUrid, sizeof(float), floatUrid, &optionSampleRate_},
      {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL},
  };
  for (int i = 0; i < 4; ++i) options_[i] = options[i];

  mapFeature_.URI = LV2_URID__map;
  mapFeature_.data = urids.mapFeature();
  unmapFeature_.URI = LV2_URID__unmap;
  unmapFeature_.data = urids.unmapFeature();
  boundedFeature_.URI = LV2_BUF_SIZE__boundedBlockLength;
  boundedFeature_.data = NULL;
  optionsFeature_.URI = LV2_OPTIONS__options;
  optionsFeature_.data = options_;
  workerFeature_.URI = LV2_WORKER__schedule;
  workerFeature_.data = &schedule_;
  features_[0] = &mapFeature_;
  features_[1] = &unmapFeature_;
  features_[2] = &boundedFeature_;
  features_[3] = &optionsFeature_;
  features_[4] = &workerFeature_;
  features_[5] = NULL;

  plugin_ = plugin;
  uri_ = uri;
  instance_ = lilv_plugin_instantiate(plugin, sampleRate, features_);
  if (!instance_) {
    std::fprintf(stderr, "lv2: %s would not instantiate\n", name_.c_str());
    return false;
  }

  // Sort the ports out: audio out into our buffers, control in at its default, and the atom
  // port that takes MIDI.
  const uint32_t portCount = lilv_plugin_get_num_ports(plugin);
  controls_.assign(portCount, 0.0f);
  std::vector<float> mins(portCount), maxes(portCount), defaults(portCount);
  lilv_plugin_get_port_ranges_float(plugin, &mins[0], &maxes[0], &defaults[0]);

  LilvNode* audioClass = lilv_new_uri(host.world(), LV2_CORE__AudioPort);
  LilvNode* controlClass = lilv_new_uri(host.world(), LV2_CORE__ControlPort);
  LilvNode* atomClass = lilv_new_uri(host.world(), LV2_ATOM__AtomPort);
  LilvNode* inputClass = lilv_new_uri(host.world(), LV2_CORE__InputPort);

  silence_.assign(maxBlockFrames, 0.0f);
  atomIn_.assign(kAtomBufferBytes, 0);
  atomOut_.assign(kAtomBufferBytes, 0);

  for (uint32_t i = 0; i < portCount; ++i) {
    const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
    const bool isInput = lilv_port_is_a(plugin, port, inputClass);
    if (lilv_port_is_a(plugin, port, audioClass)) {
      if (isInput) {
        lilv_instance_connect_port(instance_, i, &silence_[0]);
      } else {
        audioOut_.push_back(i);
      }
    } else if (lilv_port_is_a(plugin, port, controlClass)) {
      controls_[i] = defaults[i] == defaults[i] ? defaults[i] : 0.0f;  // NaN means no default
      lilv_instance_connect_port(instance_, i, &controls_[i]);
      if (isInput) {
        const LilvNode* symbol = lilv_port_get_symbol(plugin, port);
        if (symbol) controlPorts_[lilv_node_as_string(symbol)] = i;
      }
    } else if (lilv_port_is_a(plugin, port, atomClass)) {
      if (isInput && !hasAtomIn_) {
        hasAtomIn_ = true;
        lilv_instance_connect_port(instance_, i, &atomIn_[0]);
      } else {
        lilv_instance_connect_port(instance_, i, &atomOut_[0]);
      }
    } else {
      lilv_instance_connect_port(instance_, i, NULL);
    }
  }

  outputs_.resize(audioOut_.size());
  for (size_t i = 0; i < audioOut_.size(); ++i) {
    outputs_[i].assign(maxBlockFrames, 0.0f);
    lilv_instance_connect_port(instance_, audioOut_[i], &outputs_[i][0]);
  }

  lilv_node_free(audioClass);
  lilv_node_free(controlClass);
  lilv_node_free(atomClass);
  lilv_node_free(inputClass);

  if (audioOut_.empty()) {
    std::fprintf(stderr, "lv2: %s has no audio output\n", name_.c_str());
    return false;
  }

  // What the plugin says it can be told: patch:writable properties, and their value types.
  LilvNode* writable = lilv_new_uri(host.world(), LV2_PATCH__writable);
  LilvNode* range = lilv_new_uri(host.world(), "http://www.w3.org/2000/01/rdf-schema#range");
  LilvNodes* properties = lilv_plugin_get_value(plugin, writable);
  if (properties) {
    LILV_FOREACH(nodes, it, properties) {
      const LilvNode* property = lilv_nodes_get(properties, it);
      const char* uri = lilv_node_as_uri(property);
      if (!uri) continue;
      Parameter parameter;
      parameter.name = lastUriPart(uri);
      parameter.property = urids.map(uri);
      parameter.range = 0;
      LilvNodes* ranges = lilv_world_find_nodes(host.world(), property, range, NULL);
      if (ranges) {
        LilvIter* first = lilv_nodes_begin(ranges);
        if (!lilv_nodes_is_end(ranges, first)) {
          const char* rangeUri = lilv_node_as_uri(lilv_nodes_get(ranges, first));
          if (rangeUri) parameter.range = urids.map(rangeUri);
        }
        lilv_nodes_free(ranges);
      }
      parameters_.push_back(parameter);
    }
    lilv_nodes_free(properties);
  }
  lilv_node_free(writable);
  lilv_node_free(range);

  // The worker, for plugins that load files or otherwise can't work on the audio thread.
  workerInterface_ = static_cast<const LV2_Worker_Interface*>(
      lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
  if (workerInterface_) {
    workerRunning_.store(true);
    worker_ = std::thread(&Lv2Plugin::runWorker, this);
  }

  lv2_atom_forge_init(&forge_, urids.mapFeature());
  return true;
}

// Saving and restoring state: the control ports go through these, everything else is the
// plugin's own business.
const void* Lv2Plugin::getPortValue(const char* symbol, void* handle, uint32_t* size,
                                    uint32_t* type) {
  Lv2Plugin* self = static_cast<Lv2Plugin*>(handle);
  std::map<std::string, uint32_t>::const_iterator port = self->controlPorts_.find(symbol);
  if (port == self->controlPorts_.end()) {
    *size = 0;
    *type = 0;
    return NULL;
  }
  *size = sizeof(float);
  *type = self->floatUrid_;
  return &self->controls_[port->second];
}

void Lv2Plugin::setPortValue(const char* symbol, void* handle, const void* value, uint32_t size,
                             uint32_t type) {
  Lv2Plugin* self = static_cast<Lv2Plugin*>(handle);
  if (size != sizeof(float) || type != self->floatUrid_) return;
  std::map<std::string, uint32_t>::const_iterator port = self->controlPorts_.find(symbol);
  if (port != self->controlPorts_.end()) {
    self->controls_[port->second] = *static_cast<const float*>(value);
  }
}

std::string Lv2Plugin::saveState() const {
  if (!instance_ || !plugin_) return std::string();
  Lv2Plugin* self = const_cast<Lv2Plugin*>(this);
  LilvState* state = lilv_state_new_from_instance(
      plugin_, instance_, host_->urids().mapFeature(), NULL, NULL, NULL, NULL, getPortValue, self,
      LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features_);
  if (!state) return std::string();
  char* text = lilv_state_to_string(host_->world(), host_->urids().mapFeature(),
                                    host_->urids().unmapFeature(), state,
                                    "urn:groovix:state", NULL);
  std::string out = text ? text : "";
  if (text) lilv_free(text);
  lilv_state_free(state);
  return out;
}

bool Lv2Plugin::restoreState(const std::string& text) {
  if (!instance_ || text.empty()) return false;
  LilvState* state =
      lilv_state_new_from_string(host_->world(), host_->urids().mapFeature(), text.c_str());
  if (!state) return false;
  lilv_state_restore(state, instance_, setPortValue, this, 0, features_);
  lilv_state_free(state);
  return true;
}

bool Lv2Plugin::setParameter(const std::string& name, const std::string& value) {
  std::map<std::string, uint32_t>::const_iterator port = controlPorts_.find(name);
  if (port != controlPorts_.end()) {
    char* end = NULL;
    const double number = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') return false;
    controls_[port->second] = static_cast<float>(number);
    return true;
  }

  for (size_t i = 0; i < parameters_.size(); ++i) {
    if (parameters_[i].name != name) continue;
    PendingParameter pending;
    pending.property = parameters_[i].property;
    pending.type = parameters_[i].range;
    pending.number = 0.0f;
    pending.text[0] = '\0';
    if (pending.type == pathUrid_ || pending.type == stringUrid_) {
      if (value.size() >= sizeof(pending.text)) return false;
      std::memcpy(pending.text, value.c_str(), value.size() + 1);
    } else {
      char* end = NULL;
      const double number = std::strtod(value.c_str(), &end);
      if (end == value.c_str() || *end != '\0') return false;
      pending.number = static_cast<float>(number);
    }
    return pending_.push(pending);  // sent in the next block
  }
  return false;
}

// A plugin can have hundreds of these (sfizz has one per MIDI CC), so only enough to see the
// shape of them.
std::string Lv2Plugin::parameterNames() const {
  const size_t kShow = 12;
  std::vector<std::string> all;
  for (size_t i = 0; i < parameters_.size(); ++i) all.push_back(parameters_[i].name);
  for (std::map<std::string, uint32_t>::const_iterator it = controlPorts_.begin();
       it != controlPorts_.end(); ++it) {
    all.push_back(it->first);
  }
  std::string names;
  for (size_t i = 0; i < all.size() && i < kShow; ++i) {
    if (!names.empty()) names += ", ";
    names += all[i];
  }
  if (all.size() > kShow) {
    char more[48];
    std::snprintf(more, sizeof(more), " and %zu more", all.size() - kShow);
    names += more;
  }
  return names;
}

void Lv2Plugin::activate() {
  if (instance_) lilv_instance_activate(instance_);
}

void Lv2Plugin::deactivate() {
  if (instance_) lilv_instance_deactivate(instance_);
}

// ---- The worker: the plugin asks for something slow, we do it off the audio thread ----

LV2_Worker_Status Lv2Plugin::scheduleWork(LV2_Worker_Schedule_Handle handle, uint32_t size,
                                          const void* data) {
  Lv2Plugin* self = static_cast<Lv2Plugin*>(handle);
  if (size > sizeof(WorkMessage::data)) {
    self->droppedWork_.fetch_add(1, std::memory_order_relaxed);
    return LV2_WORKER_ERR_NO_SPACE;
  }
  WorkMessage message;
  message.size = size;
  std::memcpy(message.data, data, size);
  if (!self->requests_.push(message)) {
    self->droppedWork_.fetch_add(1, std::memory_order_relaxed);
    return LV2_WORKER_ERR_NO_SPACE;
  }
  self->workSignal_.notify_one();
  return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Lv2Plugin::respond(LV2_Worker_Respond_Handle handle, uint32_t size,
                                     const void* data) {
  Lv2Plugin* self = static_cast<Lv2Plugin*>(handle);
  if (size > sizeof(WorkMessage::data)) return LV2_WORKER_ERR_NO_SPACE;
  WorkMessage message;
  message.size = size;
  std::memcpy(message.data, data, size);
  return self->responses_.push(message) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
}

void Lv2Plugin::runWorker() {
  while (workerRunning_.load(std::memory_order_relaxed)) {
    WorkMessage message;
    if (requests_.pop(message)) {
      workerInterface_->work(lilv_instance_get_handle(instance_), respond, this, message.size,
                             message.data);
      continue;
    }
    std::unique_lock<std::mutex> lock(workMutex_);
    workSignal_.wait_for(lock, std::chrono::milliseconds(20));
  }
}

// ---- Audio thread ----

void Lv2Plugin::beginBlock() {
  if (!hasAtomIn_) return;
  // An empty sequence for this block; notes and CCs are appended to it as they arrive.
  lv2_atom_forge_set_buffer(&forge_, &atomIn_[0], atomIn_.size());
  lv2_atom_forge_sequence_head(&forge_, &frame_, 0);

  // Anything the config asked for goes in first, as a patch:Set the plugin understands.
  PendingParameter pending;
  while (pending_.pop(pending)) {
    LV2_Atom_Forge_Frame object;
    lv2_atom_forge_frame_time(&forge_, 0);
    lv2_atom_forge_object(&forge_, &object, 0, patchSetUrid_);
    lv2_atom_forge_key(&forge_, patchPropertyUrid_);
    lv2_atom_forge_urid(&forge_, pending.property);
    lv2_atom_forge_key(&forge_, patchValueUrid_);
    if (pending.type == pathUrid_) {
      lv2_atom_forge_path(&forge_, pending.text, static_cast<uint32_t>(std::strlen(pending.text)));
    } else if (pending.type == stringUrid_) {
      lv2_atom_forge_string(&forge_, pending.text,
                            static_cast<uint32_t>(std::strlen(pending.text)));
    } else if (pending.type == intUrid_) {
      lv2_atom_forge_int(&forge_, static_cast<int32_t>(pending.number));
    } else {
      lv2_atom_forge_float(&forge_, pending.number);
    }
    lv2_atom_forge_pop(&forge_, &object);
  }
}

void Lv2Plugin::appendMidi(const uint8_t* bytes, uint32_t size) {
  if (!hasAtomIn_) return;
  midiSent_.fetch_add(1, std::memory_order_relaxed);
  lv2_atom_forge_frame_time(&forge_, 0);  // everything lands at the start of the block
  lv2_atom_forge_atom(&forge_, size, midiEventUrid_);
  lv2_atom_forge_write(&forge_, bytes, size);
}

void Lv2Plugin::noteOn(uint8_t note, uint8_t velocity) {
  const uint8_t message[3] = {0x90, static_cast<uint8_t>(note & 0x7F),
                              static_cast<uint8_t>(velocity & 0x7F)};
  appendMidi(message, sizeof(message));
}

void Lv2Plugin::noteOff(uint8_t note) {
  const uint8_t message[3] = {0x80, static_cast<uint8_t>(note & 0x7F), 0};
  appendMidi(message, sizeof(message));
}

void Lv2Plugin::controlChange(uint8_t cc, uint8_t value) {
  const uint8_t message[3] = {0xB0, static_cast<uint8_t>(cc & 0x7F),
                              static_cast<uint8_t>(value & 0x7F)};
  appendMidi(message, sizeof(message));
}

void Lv2Plugin::render(float* left, float* right, uint32_t frames) {
  if (!instance_ || frames > maxBlock_) return;
  if (hasAtomIn_) lv2_atom_forge_pop(&forge_, &frame_);

  // An atom output port must say how much room it has before every run.
  if (!atomOut_.empty()) {
    LV2_Atom_Sequence* out = reinterpret_cast<LV2_Atom_Sequence*>(&atomOut_[0]);
    out->atom.size = static_cast<uint32_t>(atomOut_.size() - sizeof(LV2_Atom));
    out->atom.type = atomSequenceUrid_;
  }

  // Anything the worker finished goes in before the plugin runs again.
  if (workerInterface_) {
    WorkMessage message;
    while (responses_.pop(message)) {
      if (workerInterface_->work_response) {
        workerInterface_->work_response(lilv_instance_get_handle(instance_), message.size,
                                        message.data);
      }
    }
    if (workerInterface_->end_run) workerInterface_->end_run(lilv_instance_get_handle(instance_));
  }

  lilv_instance_run(instance_, frames);
  blocksRun_.fetch_add(1, std::memory_order_relaxed);

  // Mono plugins land in both speakers; anything wider than stereo keeps its first two.
  const float* outLeft = &outputs_[0][0];
  const float* outRight = outputs_.size() > 1 ? &outputs_[1][0] : outLeft;
  for (uint32_t i = 0; i < frames; ++i) {
    left[i] += outLeft[i];
    right[i] += outRight[i];
  }
}

}  // namespace gx
