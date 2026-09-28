#include "ui/ControlMap.h"

#include <string.h>

#include "engine/Project.h"

namespace gx {

namespace {

// Default CCs: the usual mixer and synth assignments, so a fresh build is useful without a
// config file.
const uint8_t kDefaultFaderCc = 11;      // Expression, for the faders under the pads
const uint8_t kDefaultMixFaderCc = 7;    // Volume
const uint8_t kDefaultKnobCc[kKnobRows] = {74, 71, 10};  // Cutoff, Resonance, Pan
const uint8_t kCentreDetent = kFaderMax / 32;  // positions this close to the middle send 64

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Compares a piece of text with a name, case-insensitively.
bool equals(const char* text, size_t length, const char* name) {
  size_t i = 0;
  for (; i < length; ++i) {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (name[i] == '\0' || name[i] != c) return false;
  }
  return name[i] == '\0';
}

// Reads a number at the start of text. Returns false if there are no digits.
bool parseUint(const char* text, size_t length, size_t& at, uint16_t& out) {
  if (at >= length || text[at] < '0' || text[at] > '9') return false;
  uint32_t value = 0;
  for (; at < length && text[at] >= '0' && text[at] <= '9'; ++at) {
    value = value * 10 + static_cast<uint32_t>(text[at] - '0');
    if (value > 0xFFFF) return false;
  }
  out = static_cast<uint16_t>(value);
  return true;
}

// A name with a trailing number, e.g. "fader3" -> prefix "fader", number 3.
bool parseNumbered(const char* text, size_t length, const char* prefix, uint16_t& number) {
  const size_t prefixLength = strlen(prefix);
  if (length <= prefixLength || !equals(text, prefixLength, prefix)) return false;
  size_t at = prefixLength;
  return parseUint(text, length, at, number) && at == length;
}

}  // namespace

ControlMap::ControlMap() { reset(); }

void ControlMap::reset() {
  none_.cc = kNoCc;
  none_.sweep = kSweepFull;
  for (uint8_t i = 0; i < kNumTrackFaders; ++i) {
    faders_[i].cc = kDefaultFaderCc;
    faders_[i].sweep = kSweepFull;
  }
  master_ = none_;
  for (uint8_t i = 0; i < kNumMixStrips; ++i) {
    mixFaders_[i].cc = kDefaultMixFaderCc;
    mixFaders_[i].sweep = kSweepFull;
  }
  mixMaster_ = none_;
  for (uint8_t row = 0; row < kKnobRows; ++row) {
    for (uint8_t strip = 0; strip < kNumMixStrips; ++strip) {
      CcAssignment& knob = knobs_[knobIndex(row, strip)];
      knob.cc = kDefaultKnobCc[row];
      // Pan is the one that wants a centre.
      knob.sweep = kDefaultKnobCc[row] == 10 ? kSweepCentre : kSweepFull;
    }
  }
}

const CcAssignment& ControlMap::assignment(uint8_t group, uint8_t index) const {
  ControlMap* self = const_cast<ControlMap*>(this);
  const CcAssignment* found = self->slot(group, index);
  return found ? *found : none_;
}

CcAssignment* ControlMap::slot(uint8_t group, uint8_t index) {
  switch (group) {
    case kGroupFader:
      return index < kNumTrackFaders ? &faders_[index] : NULL;
    case kGroupMasterFader:
      return index == 0 ? &master_ : NULL;
    case kGroupMixFader:
      return index < kNumMixStrips ? &mixFaders_[index] : NULL;
    case kGroupMixMaster:
      return index == 0 ? &mixMaster_ : NULL;
    case kGroupKnob:
      return index < kNumKnobs ? &knobs_[index] : NULL;
    default:
      return NULL;
  }
}

uint8_t ControlMap::slotIndex(uint8_t group, uint8_t index) {
  switch (group) {
    case kGroupFader:
      return index < kNumTrackFaders ? index : kNoCcSlot;
    case kGroupMasterFader:
      return index == 0 ? kNumTrackFaders : kNoCcSlot;
    case kGroupMixFader:
      return index < kNumMixStrips ? static_cast<uint8_t>(kNumTrackFaders + 1 + index)
                                   : kNoCcSlot;
    case kGroupMixMaster:
      return index == 0 ? static_cast<uint8_t>(kNumTrackFaders + 1 + kNumMixStrips) : kNoCcSlot;
    case kGroupKnob:
      return index < kNumKnobs
                 ? static_cast<uint8_t>(kNumTrackFaders + kNumMixStrips + 2 + index)
                 : kNoCcSlot;
    default:
      return kNoCcSlot;
  }
}

uint8_t ControlMap::ccValue(const CcAssignment& assignment, uint16_t position) {
  if (position > kFaderMax) position = kFaderMax;
  if (assignment.sweep == kSweepCentre) {
    const uint16_t centre = kFaderMax / 2;
    if (position + kCentreDetent >= centre && position <= centre + kCentreDetent) {
      return kCcCentre;  // the detent holds the middle, which a mouse or hand rarely hits
    }
  }
  return static_cast<uint8_t>((position * kMaxCcNumber + kFaderMax / 2) / kFaderMax);
}

bool ControlMap::load(const char* text, size_t length, uint16_t* errorLine) {
  uint16_t lineNumber = 0;
  size_t at = 0;
  while (at <= length) {
    // Take the next line, without its comment.
    size_t end = at;
    while (end < length && text[end] != '\n') ++end;
    size_t stop = end;
    for (size_t i = at; i < stop; ++i) {
      if (text[i] == '#' || text[i] == ';') {
        stop = i;
        break;
      }
    }
    ++lineNumber;

    size_t first = at;
    while (first < stop && isSpace(text[first])) ++first;
    while (stop > first && isSpace(text[stop - 1])) --stop;

    if (first < stop && !applyLine(text + first, stop - first)) {
      if (errorLine) *errorLine = lineNumber;
      return false;
    }
    if (end >= length) break;
    at = end + 1;
  }
  return true;
}

// One "name = cc[, mode]" line, already trimmed and without its comment.
// Keys the platform reads from the same file for itself: midiout and midiin, which name the
// devices to send to and listen to, and p1..p8, which say where each MIDI port goes. They are not controls, so the map passes over
// them rather than calling the line a mistake.
static bool isPlatformKey(const char* name, size_t length) {
  if (length == 2 && (name[0] == 'p' || name[0] == 'P') && name[1] >= '1' &&
      name[1] <= '0' + static_cast<char>(kNumMidiPorts)) {
    return true;
  }
  static const char* const kKeys[] = {"midiout", "midiin"};
  for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
    size_t n = 0;
    while (kKeys[k][n] != '\0') ++n;
    if (n != length) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; ++i) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      same = c == kKeys[k][i];
    }
    if (same) return true;
  }
  return false;
}

bool ControlMap::applyLine(const char* line, size_t length) {
  size_t equalsAt = 0;
  while (equalsAt < length && line[equalsAt] != '=') ++equalsAt;
  if (equalsAt == length) return false;

  size_t nameEnd = equalsAt;
  while (nameEnd > 0 && isSpace(line[nameEnd - 1])) --nameEnd;
  size_t valueAt = equalsAt + 1;
  while (valueAt < length && isSpace(line[valueAt])) ++valueAt;
  if (nameEnd == 0 || valueAt >= length) return false;

  if (isPlatformKey(line, nameEnd)) return true;  // not ours; the platform picks it up

  uint8_t group = 0;
  uint8_t firstIndex = 0;
  uint8_t count = 0;
  if (!parseName(line, nameEnd, group, firstIndex, count)) return false;

  CcAssignment assignment;
  if (!parseValue(line + valueAt, length - valueAt, assignment)) return false;
  for (uint8_t i = 0; i < count; ++i) {
    CcAssignment* target = slot(group, static_cast<uint8_t>(firstIndex + i));
    if (target) *target = assignment;
  }
  return true;
}

// A control name, which may stand for a whole row of eight.
bool ControlMap::parseName(const char* name, size_t length, uint8_t& group, uint8_t& firstIndex,
                           uint8_t& count) {
  uint16_t number = 0;
  count = 1;
  if (equals(name, length, "master")) {
    group = kGroupMasterFader;
    firstIndex = 0;
    return true;
  }
  if (equals(name, length, "mixmaster")) {
    group = kGroupMixMaster;
    firstIndex = 0;
    return true;
  }
  if (equals(name, length, "faderrow") || equals(name, length, "mixfaderrow")) {
    group = equals(name, length, "faderrow") ? kGroupFader : kGroupMixFader;
    firstIndex = 0;
    count = group == kGroupFader ? kNumTrackFaders : kNumMixStrips;
    return true;
  }
  if (parseNumbered(name, length, "mixfader", number)) {
    if (number < 1 || number > kNumMixStrips) return false;
    group = kGroupMixFader;
    firstIndex = static_cast<uint8_t>(number - 1);
    return true;
  }
  if (parseNumbered(name, length, "fader", number)) {
    if (number < 1 || number > kNumTrackFaders) return false;
    group = kGroupFader;
    firstIndex = static_cast<uint8_t>(number - 1);
    return true;
  }
  if (parseNumbered(name, length, "knobrow", number)) {
    if (number < 1 || number > kKnobRows) return false;
    group = kGroupKnob;
    firstIndex = knobIndex(static_cast<uint8_t>(number - 1), 0);
    count = kNumMixStrips;  // a knob row is eight strips, side by side in the table
    return true;
  }
  // knob<row>.<strip>
  const size_t prefix = 4;  // "knob"
  if (length > prefix && equals(name, prefix, "knob")) {
    size_t at = prefix;
    uint16_t row = 0;
    uint16_t strip = 0;
    if (!parseUint(name, length, at, row) || at >= length || name[at] != '.') return false;
    ++at;
    if (!parseUint(name, length, at, strip) || at != length) return false;
    if (row < 1 || row > kKnobRows || strip < 1 || strip > kNumMixStrips) return false;
    group = kGroupKnob;
    firstIndex = knobIndex(static_cast<uint8_t>(row - 1), static_cast<uint8_t>(strip - 1));
    return true;
  }
  return false;
}

// "off", or a CC number with an optional sweep.
bool ControlMap::parseValue(const char* value, size_t length, CcAssignment& out) {
  out.sweep = kSweepFull;
  if (equals(value, length, "off") || equals(value, length, "none")) {
    out.cc = kNoCc;
    return true;
  }
  size_t at = 0;
  uint16_t cc = 0;
  if (!parseUint(value, length, at, cc) || cc > kMaxCcNumber) return false;
  out.cc = static_cast<uint8_t>(cc);

  while (at < length && isSpace(value[at])) ++at;
  if (at == length) return true;
  if (value[at] != ',') return false;
  ++at;
  while (at < length && isSpace(value[at])) ++at;
  size_t end = length;
  while (end > at && isSpace(value[end - 1])) --end;
  const size_t modeLength = end - at;
  if (equals(value + at, modeLength, "full")) return true;
  if (equals(value + at, modeLength, "center") || equals(value + at, modeLength, "centre")) {
    out.sweep = kSweepCentre;
    return true;
  }
  return false;
}

}  // namespace gx
