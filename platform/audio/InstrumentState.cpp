#include "audio/InstrumentState.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace gx {

namespace {

const uint8_t kMagic[4] = {'G', 'X', 'I', 'S'};
const uint8_t kVersion = 1;
const size_t kHeaderSize = 6;  // magic, version, count
// Plugin state is text: a sampler's is a path, a synth's a list of numbers. Generous, but a
// plugin that wants megabytes of state is one we can't keep with a project anyway.
const size_t kMaxBlobSize = 1024 * 1024;

void putU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

InstrumentState::InstrumentState(Storage& storage, InstrumentRack& rack)
    : storage_(storage), rack_(rack), saved_(0), restored_(0) {}

void InstrumentState::keyFor(uint16_t slot, char* key) {
  std::snprintf(key, 24, "instruments_%03u", static_cast<unsigned>(slot));
}

void InstrumentState::saveProject(uint16_t slot) {
  saved_ = 0;
  std::vector<uint8_t> blob;
  blob.insert(blob.end(), kMagic, kMagic + sizeof(kMagic));
  blob.push_back(kVersion);
  blob.push_back(0);  // how many slots follow, filled in below

  const std::vector<InstrumentRack::SlotState> states = rack_.saveAll();
  for (size_t i = 0; i < states.size(); ++i) {
    const InstrumentRack::SlotState& slotState = states[i];
    if (slotState.pluginUri.empty() || slotState.state.empty()) continue;

    blob.push_back(slotState.slot);
    putU16(blob, static_cast<uint16_t>(slotState.pluginUri.size()));
    putU32(blob, static_cast<uint32_t>(slotState.state.size()));
    blob.insert(blob.end(), slotState.pluginUri.begin(), slotState.pluginUri.end());
    blob.insert(blob.end(), slotState.state.begin(), slotState.state.end());
    ++saved_;
  }

  char key[24];
  keyFor(slot, key);
  if (saved_ == 0) {
    storage_.remove(key);  // nothing to remember about this project
    return;
  }
  blob[5] = saved_;
  if (blob.size() > kMaxBlobSize || !storage_.write(key, &blob[0], blob.size())) {
    std::fprintf(stderr, "instruments: could not save the plugin settings for project %u\n",
                 static_cast<unsigned>(slot + 1));
  }
}

void InstrumentState::openProject(uint16_t slot) {
  restored_ = 0;
  if (rack_.pluginCount() == 0) return;  // nothing loaded to restore into

  char key[24];
  keyFor(slot, key);
  if (!storage_.exists(key)) return;
  if (buffer_.size() < kMaxBlobSize) buffer_.resize(kMaxBlobSize);

  size_t size = 0;
  if (!storage_.read(key, &buffer_[0], buffer_.size(), size) || size < kHeaderSize) return;
  if (std::memcmp(&buffer_[0], kMagic, sizeof(kMagic)) != 0) return;
  if (buffer_[4] != kVersion) return;

  const uint8_t count = buffer_[5];
  std::vector<InstrumentRack::SlotState> states;
  size_t at = kHeaderSize;
  for (uint8_t i = 0; i < count; ++i) {
    if (at + 7 > size) break;  // truncated: use what we have
    InstrumentRack::SlotState saved;
    saved.slot = buffer_[at];
    const uint16_t uriSize = readU16(&buffer_[at + 1]);
    const uint32_t stateSize = readU32(&buffer_[at + 3]);
    at += 7;
    if (at + uriSize + stateSize > size) break;

    saved.pluginUri.assign(reinterpret_cast<const char*>(&buffer_[at]), uriSize);
    saved.state.assign(reinterpret_cast<const char*>(&buffer_[at + uriSize]), stateSize);
    at += uriSize + stateSize;
    states.push_back(saved);
  }
  restored_ = rack_.restoreAll(states);
}

void InstrumentState::clearProject(uint16_t slot) {
  char key[24];
  keyFor(slot, key);
  storage_.discard(key);  // goes to the trash with the project it belongs to
}

}  // namespace gx
