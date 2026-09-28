#include "engine/Scale.h"

namespace gx {

namespace {

const uint8_t kMaxNote = 127;

struct ScaleDef {
  const char* name;
  uint8_t length;
  uint8_t intervals[12];  // semitones above the root, ascending
};

const ScaleDef kScales[kNumScales] = {
    {"MAJOR", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"MINOR", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"DORIAN", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"PHRYGIAN", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"LYDIAN", 7, {0, 2, 4, 6, 7, 9, 11}},
    {"MIXOLYDIAN", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"LOCRIAN", 7, {0, 1, 3, 5, 6, 8, 10}},
    {"HARM MINOR", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"MEL MINOR", 7, {0, 2, 3, 5, 7, 9, 11}},
    {"MAJ PENTA", 5, {0, 2, 4, 7, 9}},
    {"MIN PENTA", 5, {0, 3, 5, 7, 10}},
    {"BLUES", 6, {0, 3, 5, 6, 7, 10}},
    {"WHOLE TONE", 6, {0, 2, 4, 6, 8, 10}},
    {"DIMINISHED", 8, {0, 2, 3, 5, 6, 8, 9, 11}},  // whole-half
    {"PHRYG DOM", 7, {0, 1, 4, 5, 7, 8, 10}},
    {"HUNGARIAN", 7, {0, 2, 3, 6, 7, 8, 11}},  // Hungarian minor
    {"DBL HARMONIC", 7, {0, 1, 4, 5, 7, 8, 11}},
    {"LYDIAN DOM", 7, {0, 2, 4, 6, 7, 9, 10}},
    {"ALTERED", 7, {0, 1, 3, 4, 6, 8, 10}},
    {"NEAPOLITAN", 7, {0, 1, 3, 5, 7, 8, 11}},  // Neapolitan minor
    {"BEBOP DOM", 8, {0, 2, 4, 5, 7, 9, 10, 11}},
    {"HIRAJOSHI", 5, {0, 2, 3, 7, 8}},
    {"IN SEN", 5, {0, 1, 5, 7, 10}},
    {"CHROMATIC", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};

const char* const kRootNames[kNumRoots] = {"C",  "C#", "D",  "D#", "E",  "F",
                                           "F#", "G",  "G#", "A",  "A#", "B"};

const ScaleDef& definition(uint8_t scale) {
  return kScales[scale < kNumScales ? scale : static_cast<uint8_t>(kScaleChromatic)];
}

}  // namespace

const char* scaleName(uint8_t scale) { return definition(scale).name; }

const char* rootName(uint8_t root) { return root < kNumRoots ? kRootNames[root] : ""; }

uint8_t scaleLength(uint8_t scale) { return definition(scale).length; }

uint8_t scaleNote(uint8_t scale, uint8_t rootNote, uint8_t degree) {
  const ScaleDef& def = definition(scale);
  const unsigned note =
      rootNote + 12u * (degree / def.length) + def.intervals[degree % def.length];
  return note <= kMaxNote ? static_cast<uint8_t>(note) : kInvalidNote;
}

uint8_t scaleDegree(uint8_t scale, uint8_t rootNote, uint8_t note) {
  if (note < rootNote || note > kMaxNote) return kNoDegree;
  const ScaleDef& def = definition(scale);
  const uint8_t offset = static_cast<uint8_t>(note - rootNote);
  for (uint8_t i = 0; i < def.length; ++i) {
    if (def.intervals[i] == offset % 12) {
      return static_cast<uint8_t>(offset / 12 * def.length + i);
    }
  }
  return kNoDegree;
}

uint8_t chordNotes(uint8_t scale, uint8_t rootNote, uint8_t degree, uint8_t count,
                   uint8_t* out) {
  if (out == nullptr) return 0;
  uint8_t written = 0;
  for (uint8_t n = 0; n < count; ++n) {
    const uint16_t at = static_cast<uint16_t>(degree + 2 * n);  // every second degree: thirds
    if (at > 0xFF) break;  // a degree that high has no note anyway
    const uint8_t note = scaleNote(scale, rootNote, static_cast<uint8_t>(at));
    if (note == kInvalidNote) break;  // past the top of the MIDI range
    out[written++] = note;
  }
  return written;
}

}  // namespace gx
