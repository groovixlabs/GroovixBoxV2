#include "linux/RigConfig.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace gx {

void RigConfig::load(GroovixApp& app, const std::string& path, bool required) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    if (required) {
      std::fprintf(stderr, "controls: cannot open %s\n", path.c_str());
    } else {
      std::printf("controls: built-in defaults (no %s)\n", path.c_str());
    }
    return;
  }
  char buffer[512];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text_.append(buffer, read);
  std::fclose(file);

  uint16_t errorLine = 0;
  if (app.loadControlConfig(text_.data(), text_.size(), &errorLine)) {
    std::printf("controls: %s\n", path.c_str());
  } else {
    std::fprintf(stderr, "controls: %s line %u is not valid; the rest was applied\n",
                 path.c_str(), static_cast<unsigned>(errorLine));
  }
}

// The control map steps over the keys that belong to the platform; this picks them out. Two
// readers over one file, each ignoring what isn't theirs.
std::string RigConfig::value(const char* key) const {
  const size_t keyLength = std::strlen(key);
  size_t at = 0;
  while (at < text_.size()) {
    size_t end = text_.find('\n', at);
    if (end == std::string::npos) end = text_.size();
    size_t start = at;
    at = end + 1;
    while (start < end && std::isspace(static_cast<unsigned char>(text_[start]))) ++start;
    if (start >= end || text_[start] == '#' || text_[start] == ';') continue;
    const size_t equals = text_.find('=', start);
    if (equals == std::string::npos || equals >= end) continue;
    size_t nameEnd = equals;
    while (nameEnd > start && std::isspace(static_cast<unsigned char>(text_[nameEnd - 1]))) {
      --nameEnd;
    }
    if (nameEnd - start != keyLength) continue;
    if (strncasecmp(text_.c_str() + start, key, keyLength) != 0) continue;

    size_t valueAt = equals + 1;
    while (valueAt < end && std::isspace(static_cast<unsigned char>(text_[valueAt]))) ++valueAt;
    size_t valueEnd = end;
    // A trailing comment is not part of the value.
    for (size_t i = valueAt; i < valueEnd; ++i) {
      if (text_[i] == '#' || text_[i] == ';') {
        valueEnd = i;
        break;
      }
    }
    while (valueEnd > valueAt && std::isspace(static_cast<unsigned char>(text_[valueEnd - 1]))) {
      --valueEnd;
    }
    return text_.substr(valueAt, valueEnd - valueAt);
  }
  return std::string();
}

std::string RigConfig::portSpec(uint8_t port) const {
  if (port >= kNumMidiPorts) return std::string();
  const char key[3] = {'p', static_cast<char>('1' + port), '\0'};
  return value(key);
}

}  // namespace gx
