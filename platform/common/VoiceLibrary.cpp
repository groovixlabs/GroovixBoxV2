#include "common/VoiceLibrary.h"

#include <cstdlib>
#include <cstring>

namespace gx {

namespace {

const uint16_t kMaxVoices = 4096;  // 8 windows of 512 pads

// What a header cell has to say for us to believe a column. The cell is lowercased and every
// character that isn't a letter or a digit is dropped first, so "MSB#", "Bank MSB" and "msb"
// all arrive here as the same thing.
struct ColumnName {
  const char* text;
  uint8_t field;  // 0 msb, 1 lsb, 2 pc, 3 name
};
const ColumnName kColumnNames[] = {
    {"msb", 0},     {"bankmsb", 0},        {"cc0", 0},    {"lsb", 1},   {"banklsb", 1},
    {"cc32", 1},    {"pc", 2},             {"program", 2}, {"programchange", 2},
    {"programno", 2}, {"name", 3},         {"voice", 3},  {"voicename", 3},
};
const uint8_t kNumFields = 4;
const char* const kFieldNames[kNumFields] = {"MSB", "LSB", "PC", "Name"};

std::string normalise(const std::string& cell) {
  std::string out;
  for (std::string::size_type i = 0; i < cell.size(); ++i) {
    const char c = cell[i];
    if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
  }
  return out;
}

std::string lowered(const std::string& text) {
  std::string out = text;
  for (std::string::size_type i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
  }
  return out;
}

std::string trimmed(const std::string& text) {
  std::string::size_type first = 0, last = text.size();
  while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
  while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                          text[last - 1] == '\r')) --last;
  return text.substr(first, last - first);
}

// One CSV row. Quotes are handled because files from the wild have them, even though the one
// this was written against does not: "" inside a quoted field is a literal quote.
void splitCsv(const std::string& line, std::vector<std::string>& cells) {
  cells.clear();
  std::string cell;
  bool quoted = false;
  for (std::string::size_type i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c != '"') { cell += c; continue; }
      if (i + 1 < line.size() && line[i + 1] == '"') { cell += '"'; ++i; continue; }
      quoted = false;
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      cells.push_back(trimmed(cell));
      cell.clear();
    } else {
      cell += c;
    }
  }
  cells.push_back(trimmed(cell));
}

// A whole number and nothing else. Anything looser would read the copyright line at the foot
// of a maker's list as a voice.
bool wholeNumber(const std::string& cell, long& value) {
  if (cell.empty()) return false;
  for (std::string::size_type i = 0; i < cell.size(); ++i) {
    if (cell[i] < '0' || cell[i] > '9') return false;
  }
  value = std::strtol(cell.c_str(), NULL, 10);
  return true;
}

bool readLine(std::FILE* file, std::string& line) {
  line.clear();
  int c;
  while ((c = std::fgetc(file)) != EOF) {
    if (c == '\n') return true;
    line += static_cast<char>(c);
  }
  return !line.empty();
}

// "part of the name, ignoring case", the same rule midiout and p1..p8 match by.
bool nameContains(const std::string& whole, const std::string& part) {
  if (part.empty()) return false;
  return lowered(whole).find(lowered(part)) != std::string::npos;
}

std::string joinDir(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir[dir.size() - 1] == '/') return dir + name;
  return dir + "/" + name;
}

// A path from the config file may only name something inside the config directory: the file
// is trusted to say which list to read, not where on the disk to read from.
bool safeRelativePath(const std::string& path) {
  if (path.empty() || path[0] == '/') return false;
  return path.find("..") == std::string::npos;
}

}  // namespace

VoiceLibrary::~VoiceLibrary() {
  for (size_t i = 0; i < lists_.size(); ++i) delete lists_[i];
}

bool VoiceLibrary::readCsv(const std::string& path, List& list, std::FILE* out) const {
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (!file) {
    if (out) std::fprintf(out, "voices: cannot read '%s'\n", path.c_str());
    return false;
  }

  uint8_t column[kNumFields];
  for (uint8_t f = 0; f < kNumFields; ++f) column[f] = 0xFF;
  bool haveHeader = false;
  std::vector<std::string> cells;
  std::string line;
  uint32_t lineNumber = 0;
  bool ok = true;

  while (readLine(file, line)) {
    ++lineNumber;
    if (trimmed(line).empty()) continue;
    splitCsv(line, cells);

    if (!haveHeader) {
      // The header is what makes the file self-describing, so it is read before anything
      // else and a file without one cannot be used at all.
      for (size_t c = 0; c < cells.size(); ++c) {
        const std::string key = normalise(cells[c]);
        if (key.empty()) continue;
        for (size_t k = 0; k < sizeof(kColumnNames) / sizeof(kColumnNames[0]); ++k) {
          if (key != kColumnNames[k].text) continue;
          const uint8_t field = kColumnNames[k].field;
          if (column[field] != 0xFF) {
            if (out) {
              std::fprintf(out, "voices: '%s' line %u: two columns say %s\n", path.c_str(),
                           lineNumber, kFieldNames[field]);
            }
            ok = false;
          }
          column[field] = static_cast<uint8_t>(c);
          break;
        }
      }
      std::string missing;
      for (uint8_t f = 0; f < kNumFields; ++f) {
        if (column[f] != 0xFF) continue;
        if (!missing.empty()) missing += ", ";
        missing += kFieldNames[f];
      }
      if (!missing.empty()) {
        if (out) {
          std::fprintf(out, "voices: '%s' has no %s column - its header reads '%s'\n",
                       path.c_str(), missing.c_str(), trimmed(line).c_str());
        }
        ok = false;
      }
      if (!ok) break;
      haveHeader = true;
      continue;
    }

    long msb = 0, lsb = 0, pc = 0;
    const bool numeric =
        column[0] < cells.size() && wholeNumber(cells[column[0]], msb) &&
        column[1] < cells.size() && wholeNumber(cells[column[1]], lsb) &&
        column[2] < cells.size() && wholeNumber(cells[column[2]], pc);
    if (!numeric) {  // a heading, a blank, the copyright at the foot: not a voice
      ++list.skipped;
      continue;
    }
    if (pc == 0) {
      // Every printed voice list counts programs from 1. A zero means this one counts from 0,
      // and reading it as 1-based would put every voice in the file one place out.
      if (out) {
        std::fprintf(out, "voices: '%s' line %u: PC 0 - this list counts programs from 0, "
                          "and they must count from 1\n", path.c_str(), lineNumber);
      }
      ok = false;
      break;
    }
    if (msb > 127 || lsb > 127 || pc > 128) {
      ++list.skipped;
      continue;
    }
    if (list.entries.size() >= kMaxVoices) {
      ++list.skipped;
      continue;
    }
    VoiceAddress address;
    address.msb = static_cast<uint8_t>(msb);
    address.lsb = static_cast<uint8_t>(lsb);
    address.program = static_cast<uint8_t>(pc - 1);  // the file counts from 1, the wire from 0
    list.entries.push_back(address);
    list.names.push_back(column[3] < cells.size() ? cells[column[3]] : std::string());
  }
  std::fclose(file);

  if (ok && !haveHeader) {
    if (out) std::fprintf(out, "voices: '%s' is empty\n", path.c_str());
    ok = false;
  }
  if (ok && list.entries.empty()) {
    if (out) std::fprintf(out, "voices: '%s' has no voices in it\n", path.c_str());
    ok = false;
  }
  return ok;
}

void VoiceLibrary::load(const std::string& configDir, std::FILE* out) {
  const std::string path = joinDir(configDir, "voices.conf");
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (!file) return;  // no lists named: every device plays the built-in one

  // id.device and id.file, gathered by id. Both are values, so a device name can hold spaces,
  // slashes and punctuation without having to survive being a key or a filename.
  std::vector<std::string> ids, devices, files;
  std::string line;
  uint32_t lineNumber = 0;
  while (readLine(file, line)) {
    ++lineNumber;
    const std::string::size_type comment = line.find_first_of("#;");
    if (comment != std::string::npos) line = line.substr(0, comment);
    const std::string::size_type equals = line.find('=');
    if (equals == std::string::npos) {
      if (!trimmed(line).empty() && out) {
        std::fprintf(out, "voices: voices.conf line %u is not 'key = value'\n", lineNumber);
      }
      continue;
    }
    const std::string key = lowered(trimmed(line.substr(0, equals)));
    const std::string value = trimmed(line.substr(equals + 1));
    const std::string::size_type dot = key.rfind('.');
    if (dot == std::string::npos || value.empty()) {
      if (out) std::fprintf(out, "voices: voices.conf line %u: expected 'id.device' or "
                                 "'id.file'\n", lineNumber);
      continue;
    }
    const std::string id = key.substr(0, dot), field = key.substr(dot + 1);
    if (field != "device" && field != "file") {
      if (out) std::fprintf(out, "voices: voices.conf line %u: '%s' is not device or file\n",
                            lineNumber, field.c_str());
      continue;
    }
    size_t slot = 0;
    while (slot < ids.size() && ids[slot] != id) ++slot;
    if (slot == ids.size()) {
      ids.push_back(id);
      devices.push_back(std::string());
      files.push_back(std::string());
    }
    (field == "device" ? devices : files)[slot] = value;
  }
  std::fclose(file);

  for (size_t i = 0; i < ids.size(); ++i) {
    if (devices[i].empty() || files[i].empty()) {
      if (out) {
        std::fprintf(out, "voices: '%s' needs both %s.device and %s.file\n", ids[i].c_str(),
                     ids[i].c_str(), ids[i].c_str());
      }
      continue;
    }
    if (!safeRelativePath(files[i])) {
      if (out) {
        std::fprintf(out, "voices: '%s' must name a file inside the config directory, not "
                          "'%s'\n", ids[i].c_str(), files[i].c_str());
      }
      continue;
    }
    List* list = new List();
    list->device = devices[i];
    list->file = files[i];
    if (!readCsv(joinDir(configDir, files[i]), *list, out)) {
      if (out) {
        std::fprintf(out, "voices: %s keeps the built-in General MIDI list\n",
                     devices[i].c_str());
      }
      delete list;
      continue;
    }
    list->table = VoiceTable(&list->entries[0], static_cast<uint16_t>(list->entries.size()));
    lists_.push_back(list);
    if (out) {
      std::fprintf(out, "voices: %s - %u from %s", list->device.c_str(),
                   static_cast<unsigned>(list->entries.size()), list->file.c_str());
      if (list->skipped) std::fprintf(out, " (%u rows skipped)", list->skipped);
      std::fprintf(out, "\n");
    }
  }
}

const VoiceLibrary::List* VoiceLibrary::listForDevice(const std::string& deviceName) const {
  if (deviceName.empty()) return NULL;
  for (size_t i = 0; i < lists_.size(); ++i) {
    if (nameContains(deviceName, lists_[i]->device)) return lists_[i];
  }
  return NULL;
}

const VoiceTable& VoiceLibrary::voicesForDevice(const std::string& deviceName) const {
  const List* list = listForDevice(deviceName);
  return list ? list->table : builtIn_;
}

void VoiceLibrary::setPortDevice(uint8_t port, const std::string& deviceName) {
  if (port < kNumMidiPorts) portDevice_[port] = deviceName;
}

const std::string& VoiceLibrary::portDevice(uint8_t port) const {
  static const std::string kNone;
  return port < kNumMidiPorts ? portDevice_[port] : kNone;
}

const VoiceTable& VoiceLibrary::voicesFor(uint8_t port) const {
  return port < kNumMidiPorts ? voicesForDevice(portDevice_[port]) : builtIn_;
}

uint16_t VoiceLibrary::presetCount(uint8_t port) const { return voicesFor(port).size(); }

std::string VoiceLibrary::voiceName(uint8_t port, uint16_t slot) const {
  if (port >= kNumMidiPorts) return std::string();
  const List* list = listForDevice(portDevice_[port]);
  if (!list || slot >= list->names.size()) return std::string();
  return list->names[slot];
}

bool VoiceLibrary::findVoice(uint8_t port, const VoiceAddress& address, uint16_t& slot,
                             std::string& name) const {
  if (port >= kNumMidiPorts) return false;
  const List* list = listForDevice(portDevice_[port]);
  if (!list) {  // the built-in list: bank 0, one program per slot, and no names
    if (address.msb != 0 || address.lsb != 0) return false;
    slot = address.program;
    name.clear();
    return true;
  }
  for (size_t i = 0; i < list->entries.size(); ++i) {
    const VoiceAddress& entry = list->entries[i];
    if (entry.msb != address.msb || entry.lsb != address.lsb ||
        entry.program != address.program) {
      continue;
    }
    slot = static_cast<uint16_t>(i);
    name = list->names[i];
    return true;
  }
  return false;
}

std::string VoiceLibrary::describePort(uint8_t port) const {
  char head[16];
  std::snprintf(head, sizeof(head), "P%u ", static_cast<unsigned>(port) + 1);
  std::string out = head;
  if (port >= kNumMidiPorts || portDevice_[port].empty()) return out + "- not connected";
  out += portDevice_[port];
  const List* list = listForDevice(portDevice_[port]);
  char tail[128];
  if (!list) {
    std::snprintf(tail, sizeof(tail), " - %u built-in General MIDI",
                  static_cast<unsigned>(VoiceTable::kGeneralMidiVoices));
    return out + tail;
  }
  std::snprintf(tail, sizeof(tail), " - %u from %s",
                static_cast<unsigned>(list->entries.size()), list->file.c_str());
  out += tail;
  if (list->skipped) {
    std::snprintf(tail, sizeof(tail), " (%u rows skipped)", list->skipped);
    out += tail;
  }
  return out;
}

}  // namespace gx
