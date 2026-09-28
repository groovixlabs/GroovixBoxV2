#include "audio/InstrumentConfig.h"

#include <cstdio>
#include <cstdlib>

namespace gx {

namespace {

std::string trimmed(const std::string& text) {
  size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) return std::string();
  size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

std::string lowered(const std::string& text) {
  std::string out = text;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
  }
  return out;
}

// "i3" or "i3.gain" -> slot 2, and the field after the dot.
bool parseName(const std::string& name, uint8_t& slot, std::string& field) {
  const std::string lower = lowered(name);
  if (lower.empty() || lower[0] != 'i') return false;
  const size_t dot = lower.find('.');
  const std::string number = lower.substr(1, dot == std::string::npos ? dot : dot - 1);
  if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) return false;
  const long index = std::strtol(number.c_str(), NULL, 10);
  if (index < 1 || index > kNumInstruments) return false;
  slot = static_cast<uint8_t>(index - 1);
  field = dot == std::string::npos ? std::string() : lower.substr(dot + 1);
  return true;
}

bool parseFloat(const std::string& text, float low, float high, float& out) {
  char* end = NULL;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') return false;
  if (value < low || value > high) return false;
  out = static_cast<float>(value);
  return true;
}

}  // namespace

InstrumentConfig::InstrumentConfig() {
  none_.gain = 1.0f;
  none_.pan = 0.0f;
  for (uint8_t i = 0; i < kNumInstruments; ++i) slots_[i] = none_;
}

// The field name keeps its case for plugin parameters, which are case-sensitive.

const InstrumentSlot& InstrumentConfig::slot(uint8_t instrument) const {
  return instrument < kNumInstruments ? slots_[instrument] : none_;
}

uint8_t InstrumentConfig::assignedSlots() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < kNumInstruments; ++i) {
    if (!slots_[i].pluginUri.empty()) ++count;
  }
  return count;
}

void InstrumentConfig::applyTo(AudioEngine& engine) const {
  for (uint8_t i = 0; i < kNumInstruments; ++i) {
    engine.setGain(i, slots_[i].gain);
    engine.setPan(i, slots_[i].pan);
  }
}

bool InstrumentConfig::applyLine(const std::string& line) {
  const size_t equals = line.find('=');
  if (equals == std::string::npos) return false;
  const std::string name = trimmed(line.substr(0, equals));
  const std::string value = trimmed(line.substr(equals + 1));
  if (name.empty() || value.empty()) return false;

  uint8_t slot = 0;
  std::string field;
  if (!parseName(name, slot, field)) return false;
  // parseName lowercases; plugin parameter names keep the case they were written in.
  const size_t dot = name.find('.');
  const std::string rawField = dot == std::string::npos ? std::string() : name.substr(dot + 1);

  if (field.empty()) {
    slots_[slot].pluginUri = value;  // the plugin this slot hosts
    return true;
  }
  if (field == "gain") return parseFloat(value, 0.0f, 1.0f, slots_[slot].gain);
  if (field == "pan") return parseFloat(value, -1.0f, 1.0f, slots_[slot].pan);
  // Everything else belongs to the plugin, which says at load time whether it has it.
  slots_[slot].parameters.push_back(std::make_pair(rawField, value));
  return true;
}

bool InstrumentConfig::load(const std::string& text, uint16_t* errorLine) {
  uint16_t lineNumber = 0;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    ++lineNumber;

    const size_t comment = line.find_first_of("#;");
    if (comment != std::string::npos) line = line.substr(0, comment);
    line = trimmed(line);
    if (!line.empty() && !applyLine(line)) {
      if (errorLine) *errorLine = lineNumber;
      return false;
    }
    if (end == text.size()) break;
    at = end + 1;
  }
  return true;
}

bool InstrumentConfig::loadFile(const std::string& path, uint16_t* errorLine) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::string text;
  char buffer[512];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  std::fclose(file);
  return load(text, errorLine);
}

}  // namespace gx
