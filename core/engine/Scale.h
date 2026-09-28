#pragma once

#include <stdint.h>

namespace gx {

enum ScaleId {
  kScaleMajor = 0,
  kScaleMinor,
  kScaleDorian,
  kScalePhrygian,
  kScaleLydian,
  kScaleMixolydian,
  kScaleLocrian,
  kScaleHarmonicMinor,
  kScaleMelodicMinor,
  kScaleMajorPentatonic,
  kScaleMinorPentatonic,
  kScaleBlues,
  kScaleWholeTone,
  kScaleDiminished,
  kScalePhrygianDominant,
  kScaleHungarianMinor,
  kScaleDoubleHarmonic,
  kScaleLydianDominant,
  kScaleAltered,
  kScaleNeapolitanMinor,
  kScaleBebopDominant,
  kScaleHirajoshi,
  kScaleInSen,
  kScaleChromatic,
  kNumScales,
};

static const uint8_t kNumRoots = 12;  // C, C#, D ... B
static const uint8_t kNoDegree = 0xFF;
static const uint8_t kInvalidNote = 0xFF;

// MIDI note of C in an octave: octave 2 is C2, MIDI 36.
inline uint8_t octaveBaseNote(uint8_t octave) { return static_cast<uint8_t>(12 * (octave + 1)); }

// Short display name, e.g. "MAJOR", "HARM MINOR".
const char* scaleName(uint8_t scale);
// Note name of a root, e.g. "C#".
const char* rootName(uint8_t root);
// Notes per octave.
uint8_t scaleLength(uint8_t scale);
// The degree-th note of a scale counted up from rootNote (degree 0 is rootNote), or
// kInvalidNote above MIDI note 127. Unknown scales behave as chromatic.
uint8_t scaleNote(uint8_t scale, uint8_t rootNote, uint8_t degree);
// Degree of a note in a scale that starts at rootNote, or kNoDegree if the note is below
// rootNote or not in the scale.
uint8_t scaleDegree(uint8_t scale, uint8_t rootNote, uint8_t note);
// The notes of the chord built on a scale degree: the degree itself and the rest two scale
// steps apart, which is a third in the scale's own terms, so the chord always belongs to the
// scale. Writes up to count notes into out, stopping at the top of the MIDI range, and
// returns how many it wrote.
uint8_t chordNotes(uint8_t scale, uint8_t rootNote, uint8_t degree, uint8_t count, uint8_t* out);

}  // namespace gx
