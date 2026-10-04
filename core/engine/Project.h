#pragma once

#include <stdint.h>

#include "engine/Scale.h"

namespace gx {

// Capacity limits are chosen per platform build by defining GX_NUM_TRACKS and
// GX_NUM_PATTERNS. The defaults keep a project small enough for an MCU without external
// RAM (about 25 KB); desktop builds raise them to fill the UI's 8 pages (64 each). At 64
// tracks, 64 patterns and 256 steps a Project is about 11 MB, and the app holds two of them -
// the open one and a scratch copy - so roughly 22 MB. Fine on a desktop, nowhere near an MCU.
#ifndef GX_NUM_TRACKS
#define GX_NUM_TRACKS 16
#endif
#ifndef GX_NUM_PATTERNS
#define GX_NUM_PATTERNS 16
#endif
#ifndef GX_MAX_STEPS
#define GX_MAX_STEPS 64
#endif
static_assert(GX_NUM_TRACKS >= 1 && GX_NUM_TRACKS <= 255, "GX_NUM_TRACKS must be 1..255");
static_assert(GX_NUM_PATTERNS >= 1 && GX_NUM_PATTERNS <= 255, "GX_NUM_PATTERNS must be 1..255");
static_assert(GX_MAX_STEPS >= 1 && GX_MAX_STEPS <= 256, "GX_MAX_STEPS must be 1..256");

static const uint8_t kNumTracks = GX_NUM_TRACKS;
static const uint8_t kNumPatterns = GX_NUM_PATTERNS;  // per track
static const uint16_t kMaxSteps = GX_MAX_STEPS;
// New patterns are one gridful of steps long; Shift + a step pad extends them.
static const uint16_t kDefaultPatternLength = kMaxSteps < 32 ? kMaxSteps : 32;
static const uint16_t kMinBpm = 20;
static const uint16_t kMaxBpm = 300;
static const uint16_t kDefaultBpm = 120;
static const uint8_t kDefaultVelocity = 100;
static const uint8_t kMaxProbability = 100;  // percent: the step always plays
// Playback runs in ticks, 24 per step: 96 per quarter note, the resolution a groovebox needs
// to place a note off the grid. MIDI clock is every 4th tick, which is its usual 24 a quarter.
static const uint8_t kTicksPerStep = 24;
static const uint8_t kTicksPerMidiClock = 4;
// A step's gate, how long its notes sound, is counted in sixths of a step: from a sixth up to
// 16 steps. It is coarser than a tick on purpose — it is what the pads offer.
static const uint8_t kGateUnitsPerStep = 6;
static const uint8_t kTicksPerGateUnit = kTicksPerStep / kGateUnitsPerStep;
static const uint8_t kMaxGateUnits = 16 * kGateUnitsPerStep;
static const uint8_t kDefaultGateUnits = kGateUnitsPerStep;  // one step
// Swing: how much of each pair of steps the first one takes, as a percentage. 50 is straight
// and 75 puts the second step half a step late, a hard shuffle.
static const uint8_t kMinSwing = 50;
static const uint8_t kMaxSwing = 75;
static const uint8_t kDefaultSwing = kMinSwing;
static const uint8_t kMaxStepNotes = 4;  // notes a step can play at once: a chord
// Micro-timing: a step can play early or late by up to a third of a step, in ticks. Sixteen
// values, one per pad of a lane, with straight in the middle.
// Half a step either way. Every 8th- and 16th-note triplet position in a beat is within 8
// ticks of a step, so a range this wide puts all of them on the grid's doorstep; the lane
// spends its pads unevenly to cover it (see StepParamsMode).
static const int8_t kMinNudge = -12;
static const int8_t kMaxNudge = 12;
static const int8_t kDefaultNudge = 0;
// The note keyboard starts on the scale root in one of these octaves; Shift + the outer
// keyboard columns moves it. Octave 2 is C2, the bottom of the MIDI range the pads reach.
static const uint8_t kMaxKeyboardOctave = 8;
static const uint8_t kDefaultKeyboardOctave = 2;
static const uint8_t kMaxMidiValue = 127;
static const uint8_t kNumMidiChannels = 16;
// New tracks play on MIDI channels 1..16 in order, starting again after track 16.
inline uint8_t defaultMidiChannel(uint8_t track) {
  return static_cast<uint8_t>(track % kNumMidiChannels);
}
static const uint8_t kNumMidiPorts = 8;  // MIDI outputs P1..P8: 8 ports x 16 channels
// Tracks come in pages of 8 in the UI, and each page starts on its own port: tracks 1-8 on
// port 1, tracks 9-16 on port 2 and so on.
static const uint8_t kTracksPerMidiPort = 8;
inline uint8_t defaultMidiPort(uint8_t track) {
  return static_cast<uint8_t>((track / kTracksPerMidiPort) % kNumMidiPorts);
}
// Internal instruments I1..I16. A track plays either out of a MIDI port or on one of these,
// never both; kNoInstrument means it uses its MIDI port.
static const uint8_t kNumInstruments = 16;
static const uint8_t kNoInstrument = 0xFF;

// How a track's note keyboard is laid out.
enum KeyboardLayout {
  kKeyboardProjectScale = 0,  // notes of the song's scale, from its root
  kKeyboardOwnScale,          // notes of the track's own scale, which can differ from the song's
  kKeyboardDrums,             // two 4x4 blocks of drum pads, consecutive notes from C
  kNumKeyboardLayouts,
};
// Track::scale of a track without its own scale.
static const uint8_t kNoOwnScale = kNumScales;

// One key, one chord. A key of the note keyboard plays its scale degree with further notes
// stacked on top, two scale steps apart - a third in the scale's own terms - so the chord is
// always in the track's key, whatever the scale. The value is the number of notes, which is
// also how much of a step's chord it fills.
enum ChordShape {
  kChordOff = 0,     // one key, one note
  kChordTriad = 3,   // the degree, and two more
  kChordSeventh = 4,
};

// A step can fire more than once, spread evenly across its own length: two hits are
// 32nd notes, four are 64ths, and eight are a drum roll. Each hit lasts the step's gate or
// the space to the next hit, whichever is shorter, so they never run into each other.
// One key, one chord gave a step several notes; the arpeggiator gives them their turn one at
// a time. It belongs to the track, so a whole part arpeggiates without touching its steps.
enum ArpMode {
  kArpOff = 0,
  kArpUp,       // lowest to highest
  kArpDown,
  kArpUpDown,   // up then back, without playing the ends twice
  kArpDownUp,
  kArpAsPlayed,  // the order the step's notes were written in
  kArpRandom,
  kNumArpModes,
};

// How often the arp plays a note, in ticks of the 24 a step lasts: a step is a sixteenth, so
// these are the note values around it. The list is the order the pads offer them in.
static const uint8_t kNumArpRates = 8;
static const uint8_t kArpRateTicks[kNumArpRates] = {48, 32, 24, 16, 12, 8, 6, 4};
static const uint8_t kDefaultArpRate = 2;  // 24 ticks: one note a step
static const uint8_t kMaxArpOctaves = 4;
static const uint8_t kDefaultArpOctaves = 1;

static const uint8_t kMaxRatchet = 8;
static const uint8_t kDefaultRatchet = 1;  // once, on the step, as a step has always played

struct Step {
  uint8_t active;                // 0 or 1
  uint8_t noteCount;             // notes in the chord, 0 when none has been assigned yet
  uint8_t notes[kMaxStepNotes];  // in the order they were played
  uint8_t velocity;              // 0..127
  uint8_t probability;           // chance of playing in percent, 1..kMaxProbability
  uint8_t gate;                  // sixths of a step the notes sound, 1..kMaxGateUnits
  int8_t nudge;                  // ticks early or late, kMinNudge..kMaxNudge
  uint8_t ratchet;               // times it fires across the step, 1..kMaxRatchet
};

struct Pattern {
  Step steps[kMaxSteps];
  uint16_t length;  // 1..kMaxSteps
};

struct Track {
  Pattern patterns[kNumPatterns];
  uint8_t selectedPattern;  // the pattern that plays, 0..kNumPatterns-1
  uint16_t preset;          // sound preset slot
  uint8_t note;             // note given to newly enabled steps: the last one assigned
  uint8_t keyboardOctave;   // octave the note keyboard starts in, 0..kMaxKeyboardOctave
  uint8_t keyboardLayout;   // KeyboardLayout
  uint8_t scaleRoot;        // the track's own scale (kKeyboardOwnScale): 0 = C .. 11 = B
  uint8_t scale;            // ScaleId, or kNoOwnScale when the track has no own scale
  uint8_t pianoRoll;        // 1: note mode shows this track as a piano roll
  uint8_t chord;            // ChordShape: notes one key of the keyboard plays
  uint8_t arpMode;          // ArpMode: how a step's chord is spread out, kArpOff for not
  uint8_t arpRate;          // index into kArpRateTicks
  uint8_t arpOctaves;       // how many octaves it climbs, 1..kMaxArpOctaves
  uint8_t midiChannel;      // 0..15 (MIDI channels 1..16)
  uint8_t midiPort;         // 0..kNumMidiPorts-1 (ports P1..P8)
  uint8_t instrument;       // 0..kNumInstruments-1 (I1..I16), or kNoInstrument for the port
};

// The note a new track gives to steps switched on before anything else is played.
uint8_t defaultTrackNote(uint8_t track);
// Whether a step, a pattern or a track's settings are still exactly as initProject left them.
// The codec stores only what differs, so an untouched project takes almost no space.
bool isStepDefault(const Step& step);
bool isPatternDefault(const Pattern& pattern);
bool isTrackHeaderDefault(const Track& track, uint8_t index);

// A song section: which tracks are silent while it plays, and which pattern each one plays.
// Scenes are captured from what is sounding, launched from a pad, and strung together into a
// song in arrangement mode.
static const uint8_t kNumScenes = 32;
static const uint8_t kNoScene = 0xFF;

// A scene that leaves a track's pattern alone, which is what scenes saved before they
// remembered patterns do.
static const uint8_t kNoPattern = 0xFF;

struct Scene {
  uint8_t used;                  // 0 for an empty pad: a scene that mutes nothing is still one
  uint8_t muted[kNumTracks];     // 1 = that track is silent
  uint8_t patterns[kNumTracks];  // the pattern each track plays, or kNoPattern
};

// The song: scenes in the order they play. Every step lasts the same number of bars for now,
// so a longer section is the same scene in several steps; per-step lengths can come later.
// Playback counts bars as well as steps: 16 steps, which is four beats of 16th notes. It is
// the same for every track whatever their pattern lengths, so everything that should happen
// "on the bar" agrees. A Global setting later; one constant for now.
static const uint16_t kStepsPerBar = 16;

static const uint8_t kNumSongSteps = 32;
static const uint8_t kBarsPerSongStep = 4;
static const uint8_t kNoSongStep = 0xFF;

// Everything saved in a project slot. Plain data, so any storage backend or codec can handle it.
struct Project {
  Track tracks[kNumTracks];
  Scene scenes[kNumScenes];
  uint8_t song[kNumSongSteps];  // the scene each step plays, or kNoScene for an empty step
  uint16_t bpm;       // kMinBpm..kMaxBpm
  uint8_t swing;      // kMinSwing..kMaxSwing, the same groove for every track
  uint8_t scaleRoot;  // 0 = C .. 11 = B
  uint8_t scale;      // ScaleId; the note keyboard offers only notes of this scale
};

void initProject(Project& project);
void initPattern(Pattern& pattern);

// A pattern is empty when no step is active.
bool isPatternEmpty(const Pattern& pattern);
// A project is empty when it has no steps and default settings, i.e. nothing worth saving.
bool isProjectEmpty(const Project& project);

}  // namespace gx
