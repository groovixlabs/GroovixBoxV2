#include "storage/ProjectCodec.h"

#include <string.h>

namespace gx {

namespace {

const uint8_t kMagic[4] = {'G', 'X', 'P', 'J'};
const uint8_t kFormatVersion = 8;
const uint8_t kOldestVersion = 1;   // version 1 is the same file without its scenes
const uint8_t kScenesVersion = 2;
const uint8_t kSongVersion = 3;     // from here on the scenes are followed by the song
const uint8_t kScenePatternsVersion = 4;  // and each scene says which pattern a track plays
const uint8_t kSwingVersion = 5;          // and the header carries the project's swing,
                                          // and each step its micro-timing
const uint8_t kChordVersion = 6;          // and each track the chord one key plays
const uint8_t kRatchetVersion = 7;        // and each step how many times it fires
const uint8_t kArpVersion = 8;            // and each track its arpeggiator

const size_t kVersionOffset = 4;
const size_t kBpmOffset = 5;
const size_t kScaleRootOffset = 7;
const size_t kScaleOffset = 8;
const size_t kTracksOffset = 9;
const size_t kPatternsOffset = 10;
const size_t kStepsOffset = 11;
const size_t kSwingOffset = 13;  // from version 5 on

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>(value >> 8);
}

size_t bitmapSize(uint16_t count) { return (count + 7u) / 8u; }

bool bitSet(const uint8_t* bitmap, uint16_t index) {
  return (bitmap[index >> 3] >> (index & 7)) & 1;
}

void setBit(uint8_t* bitmap, uint16_t index) {
  bitmap[index >> 3] = static_cast<uint8_t>(bitmap[index >> 3] | (1u << (index & 7)));
}

// Walks a buffer without ever reading past its end: once a read doesn't fit, ok turns false
// and stays false.
struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok;

  const uint8_t* take(size_t bytes) {
    if (!ok || left < bytes) {
      ok = false;
      return NULL;
    }
    const uint8_t* at = p;
    p += bytes;
    left -= bytes;
    return at;
  }
  uint8_t byte() {
    const uint8_t* at = take(1);
    return at ? *at : 0;
  }
  uint16_t word() {
    const uint8_t* at = take(2);
    return at ? readU16(at) : 0;
  }
};

// Writes into a buffer, giving up once it would overflow.
struct Writer {
  uint8_t* p;
  size_t left;
  size_t written;
  bool ok;

  uint8_t* take(size_t bytes) {
    if (!ok || left < bytes) {
      ok = false;
      return NULL;
    }
    uint8_t* at = p;
    p += bytes;
    left -= bytes;
    written += bytes;
    return at;
  }
  void byte(uint8_t value) {
    uint8_t* at = take(1);
    if (at) *at = value;
  }
  void word(uint16_t value) {
    uint8_t* at = take(2);
    if (at) writeU16(at, value);
  }
  // Reserves a zeroed bitmap and returns where it sits, to set bits while writing entries.
  uint8_t* bitmap(size_t bytes) {
    uint8_t* at = take(bytes);
    if (at) memset(at, 0, bytes);
    return at;
  }
};

bool validStep(const uint8_t* step, bool withNudge, bool withRatchet) {
  const uint8_t active = step[0];
  const uint8_t count = step[1];
  if (active > 1 || count > kMaxStepNotes) return false;
  if (active == 1 && count == 0) return false;  // an active step always holds a note
  for (uint8_t n = 0; n < kMaxStepNotes; ++n) {
    if (step[2 + n] > kMaxMidiValue) return false;
  }
  const uint8_t velocity = step[2 + kMaxStepNotes];
  const uint8_t probability = step[3 + kMaxStepNotes];
  const uint8_t gate = step[4 + kMaxStepNotes];
  if (velocity > kMaxMidiValue) return false;
  if (probability < 1 || probability > kMaxProbability) return false;
  if (gate < 1 || gate > kMaxGateUnits) return false;
  if (withNudge) {
    const int8_t nudge = static_cast<int8_t>(step[5 + kMaxStepNotes]);
    if (nudge < kMinNudge || nudge > kMaxNudge) return false;
  }
  if (withRatchet) {
    const uint8_t ratchet = step[6 + kMaxStepNotes];
    if (ratchet < 1 || ratchet > kMaxRatchet) return false;
  }
  return true;
}

bool validTrackHeader(const uint8_t* header, uint8_t patterns, bool withChord, bool withArp) {
  if (header[0] >= patterns) return false;               // selected pattern
  if (header[3] > kMaxMidiValue) return false;           // note
  if (header[4] > kMaxKeyboardOctave) return false;      // keyboard octave
  if (header[5] > kKeyboardDrums) return false;          // keyboard layout
  if (header[6] >= kNumRoots) return false;              // own scale root
  if (header[7] > kNoOwnScale) return false;             // own scale
  if (header[8] > 1) return false;                       // piano roll
  if (header[9] >= kNumMidiChannels) return false;       // MIDI channel
  if (header[10] >= kNumMidiPorts) return false;         // MIDI port
  const uint8_t instrument = header[11];
  if (instrument >= kNumInstruments && instrument != kNoInstrument) return false;
  if (withChord) {
    const uint8_t chord = header[12];
    if (chord != kChordOff && chord != kChordTriad && chord != kChordSeventh) return false;
  }
  if (withArp) {
    if (header[13] >= kNumArpModes) return false;
    if (header[14] >= kNumArpRates) return false;
    if (header[15] < 1 || header[15] > kMaxArpOctaves) return false;
  }
  return true;
}

void readTrackHeader(const uint8_t* header, Track& track, bool withChord, bool withArp) {
  track.selectedPattern = header[0];
  track.preset = readU16(header + 1);
  track.note = header[3];
  track.keyboardOctave = header[4];
  track.keyboardLayout = header[5];
  track.scaleRoot = header[6];
  track.scale = header[7];
  track.pianoRoll = header[8];
  track.midiChannel = header[9];
  track.midiPort = header[10];
  track.instrument = header[11];
  // A file from before one key, one chord has every track playing single notes.
  track.chord = withChord ? header[12] : static_cast<uint8_t>(kChordOff);
  // A file from before the arpeggiator has every track playing its chords whole.
  track.arpMode = withArp ? header[13] : static_cast<uint8_t>(kArpOff);
  track.arpRate = withArp ? header[14] : kDefaultArpRate;
  track.arpOctaves = withArp ? header[15] : kDefaultArpOctaves;
}

void readStep(const uint8_t* data, Step& step, bool withNudge, bool withRatchet) {
  step.active = data[0];
  step.noteCount = data[1];
  for (uint8_t n = 0; n < kMaxStepNotes; ++n) step.notes[n] = data[2 + n];
  step.velocity = data[2 + kMaxStepNotes];
  step.probability = data[3 + kMaxStepNotes];
  step.gate = data[4 + kMaxStepNotes];
  // A file from before micro-timing has steps straight on the grid, and one from before
  // ratchets has every step firing once.
  step.nudge = withNudge ? static_cast<int8_t>(data[5 + kMaxStepNotes]) : kDefaultNudge;
  step.ratchet = withRatchet ? data[6 + kMaxStepNotes] : kDefaultRatchet;
}

// One pass over a file. With project given it fills it in; without, it only checks. Tracks
// past this build's capacity are read and dropped, as the header says.
bool walk(const uint8_t* data, size_t size, Project* project) {
  if (size < kProjectHeaderSizeV4) return false;
  if (memcmp(data, kMagic, sizeof(kMagic)) != 0) return false;
  const uint8_t version = data[kVersionOffset];
  if (version < kOldestVersion || version > kFormatVersion) return false;
  // The header grew by a byte with swing, so where the tracks start depends on the version.
  const size_t headerSize = version >= kSwingVersion ? kProjectHeaderSize : kProjectHeaderSizeV4;
  if (size < headerSize) return false;
  const bool withNudge = version >= kSwingVersion;  // steps grew a byte in the same version
  const bool withChord = version >= kChordVersion;  // and track headers a byte after that
  const bool withRatchet = version >= kRatchetVersion;  // and steps another, for the ratchet
  const bool withArp = version >= kArpVersion;          // and track headers three more
  const size_t trackHeaderSize = withArp ? kEncodedTrackHeaderSize
                                 : withChord ? kEncodedTrackHeaderSizeV7
                                             : kEncodedTrackHeaderSizeV5;
  const size_t stepSize = withRatchet ? kEncodedStepSize
                          : withNudge ? kEncodedStepSizeV6
                                      : kEncodedStepSizeV4;
  const uint8_t swing = version >= kSwingVersion ? data[kSwingOffset] : kDefaultSwing;
  if (swing < kMinSwing || swing > kMaxSwing) return false;

  const uint16_t bpm = readU16(data + kBpmOffset);
  const uint8_t scaleRoot = data[kScaleRootOffset];
  const uint8_t scale = data[kScaleOffset];
  const uint8_t tracks = data[kTracksOffset];
  const uint8_t patterns = data[kPatternsOffset];
  const uint16_t steps = readU16(data + kStepsOffset);
  if (bpm < kMinBpm || bpm > kMaxBpm) return false;
  if (scaleRoot >= kNumRoots || scale >= kNumScales) return false;
  if (tracks == 0 || patterns == 0 || steps == 0) return false;
  // More patterns or steps than this build can hold would be lost without a word, so refuse.
  if (patterns > kNumPatterns || steps > kMaxSteps) return false;

  Reader in = {data + headerSize, size - headerSize, true};
  const uint8_t* trackBits = in.take(bitmapSize(tracks));
  if (!in.ok) return false;

  for (uint8_t t = 0; t < tracks; ++t) {
    if (!bitSet(trackBits, t)) continue;
    const uint8_t* header = in.take(trackHeaderSize);
    if (!in.ok || !validTrackHeader(header, patterns, withChord, withArp)) return false;
    const uint8_t* patternBits = in.take(bitmapSize(patterns));
    if (!in.ok) return false;
    const bool keep = t < kNumTracks;  // a file from a wider build keeps its first tracks
    if (keep && project) readTrackHeader(header, project->tracks[t], withChord, withArp);

    for (uint8_t p = 0; p < patterns; ++p) {
      if (!bitSet(patternBits, p)) continue;
      const uint16_t length = in.word();
      const uint8_t* stepBits = in.take(bitmapSize(steps));
      if (!in.ok || length < 1 || length > steps) return false;
      if (keep && project) project->tracks[t].patterns[p].length = length;

      for (uint16_t s = 0; s < steps; ++s) {
        if (!bitSet(stepBits, s)) continue;
        const uint8_t* step = in.take(stepSize);
        if (!in.ok || !validStep(step, withNudge, withRatchet)) return false;
        if (keep && project) {
          readStep(step, project->tracks[t].patterns[p].steps[s], withNudge, withRatchet);
        }
      }
    }
  }
  // Scenes: how many the file holds, which of them are used, and what each one silences.
  if (version >= kScenesVersion) {
    const uint8_t sceneCount = in.byte();
    if (!in.ok || sceneCount == 0 || sceneCount > kNumScenes) return false;
    const uint8_t* sceneBits = in.take(bitmapSize(sceneCount));
    if (!in.ok) return false;
    for (uint8_t s = 0; s < sceneCount; ++s) {
      if (!bitSet(sceneBits, s)) continue;
      const uint8_t* muted = in.take(4);
      if (!in.ok) return false;
      if (project) {
        project->scenes[s].used = 1;
        project->scenes[s].muted = static_cast<uint32_t>(muted[0]) |
                                   (static_cast<uint32_t>(muted[1]) << 8) |
                                   (static_cast<uint32_t>(muted[2]) << 16) |
                                   (static_cast<uint32_t>(muted[3]) << 24);
      }
      // Which pattern each track plays, for the tracks the scene has an opinion about. An
      // older file has none, and those scenes leave the patterns as they are.
      if (version >= kScenePatternsVersion) {
        const uint8_t* patternBits = in.take(bitmapSize(tracks));
        if (!in.ok) return false;
        for (uint16_t t = 0; t < tracks; ++t) {
          if (!bitSet(patternBits, t)) continue;
          const uint8_t pattern = in.byte();
          if (!in.ok || pattern >= patterns) return false;
          if (project && t < kNumTracks) project->scenes[s].patterns[t] = pattern;
        }
      }
    }
  }

  // The song: which steps hold a scene, and which scene each one plays.
  if (version >= kSongVersion) {
    const uint8_t songSteps = in.byte();
    if (!in.ok || songSteps == 0 || songSteps > kNumSongSteps) return false;
    const uint8_t* songBits = in.take(bitmapSize(songSteps));
    if (!in.ok) return false;
    for (uint8_t s = 0; s < songSteps; ++s) {
      if (!bitSet(songBits, s)) continue;
      const uint8_t scene = in.byte();
      if (!in.ok || scene >= kNumScenes) return false;
      if (project) project->song[s] = scene;
    }
  }

  if (!in.ok || in.left != 0) return false;  // trailing bytes mean it isn't what it claims

  if (project) {
    project->bpm = bpm;
    project->swing = swing;
    project->scaleRoot = scaleRoot;
    project->scale = scale;
  }
  return true;
}

}  // namespace

size_t encodeProject(const Project& project, uint8_t* out, size_t capacity) {
  Writer w = {out, capacity, 0, true};
  uint8_t* header = w.take(kProjectHeaderSize);
  if (!w.ok) return 0;
  memcpy(header, kMagic, sizeof(kMagic));
  header[kVersionOffset] = kFormatVersion;
  writeU16(header + kBpmOffset, project.bpm);
  header[kScaleRootOffset] = project.scaleRoot;
  header[kSwingOffset] = project.swing;
  header[kScaleOffset] = project.scale;
  header[kTracksOffset] = kNumTracks;
  header[kPatternsOffset] = kNumPatterns;
  writeU16(header + kStepsOffset, kMaxSteps);

  uint8_t* trackBits = w.bitmap(kTrackBitmapSize);
  if (!w.ok) return 0;

  for (uint8_t t = 0; t < kNumTracks; ++t) {
    const Track& track = project.tracks[t];
    bool anyPattern = false;
    for (uint8_t p = 0; p < kNumPatterns && !anyPattern; ++p) {
      anyPattern = !isPatternDefault(track.patterns[p]);
    }
    if (!anyPattern && isTrackHeaderDefault(track, t)) continue;  // nothing to say about it

    setBit(trackBits, t);
    uint8_t* trackHeader = w.take(kEncodedTrackHeaderSize);
    if (!w.ok) return 0;
    trackHeader[0] = track.selectedPattern;
    writeU16(trackHeader + 1, track.preset);
    trackHeader[3] = track.note;
    trackHeader[4] = track.keyboardOctave;
    trackHeader[5] = track.keyboardLayout;
    trackHeader[6] = track.scaleRoot;
    trackHeader[7] = track.scale;
    trackHeader[8] = track.pianoRoll;
    trackHeader[9] = track.midiChannel;
    trackHeader[10] = track.midiPort;
    trackHeader[11] = track.instrument;
    trackHeader[12] = track.chord;
    trackHeader[13] = track.arpMode;
    trackHeader[14] = track.arpRate;
    trackHeader[15] = track.arpOctaves;

    uint8_t* patternBits = w.bitmap(kPatternBitmapSize);
    if (!w.ok) return 0;
    for (uint8_t p = 0; p < kNumPatterns; ++p) {
      const Pattern& pattern = track.patterns[p];
      if (isPatternDefault(pattern)) continue;

      setBit(patternBits, p);
      w.word(pattern.length);
      uint8_t* stepBits = w.bitmap(kStepBitmapSize);
      if (!w.ok) return 0;
      for (uint16_t s = 0; s < kMaxSteps; ++s) {
        const Step& step = pattern.steps[s];
        if (isStepDefault(step)) continue;

        setBit(stepBits, s);
        uint8_t* out2 = w.take(kEncodedStepSize);
        if (!w.ok) return 0;
        out2[0] = step.active ? 1 : 0;
        out2[1] = step.noteCount;
        for (uint8_t n = 0; n < kMaxStepNotes; ++n) out2[2 + n] = step.notes[n];
        out2[2 + kMaxStepNotes] = step.velocity;
        out2[3 + kMaxStepNotes] = step.probability;
        out2[4 + kMaxStepNotes] = step.gate;
        out2[5 + kMaxStepNotes] = static_cast<uint8_t>(step.nudge);
        out2[6 + kMaxStepNotes] = step.ratchet;
      }
    }
  }
  // Scenes, in the same shape: a bitmap, then only the ones that hold something.
  w.byte(kNumScenes);
  uint8_t* sceneBits = w.bitmap(bitmapSize(kNumScenes));
  if (!w.ok) return 0;
  for (uint8_t s = 0; s < kNumScenes; ++s) {
    if (!project.scenes[s].used) continue;
    setBit(sceneBits, s);
    uint8_t* muted = w.take(4);
    if (!w.ok) return 0;
    for (int i = 0; i < 4; ++i) {
      muted[i] = static_cast<uint8_t>((project.scenes[s].muted >> (8 * i)) & 0xFF);
    }
    uint8_t* patternBits = w.bitmap(bitmapSize(kNumTracks));
    if (!w.ok) return 0;
    for (uint8_t t = 0; t < kNumTracks; ++t) {
      if (project.scenes[s].patterns[t] == kNoPattern) continue;
      setBit(patternBits, t);
      w.byte(project.scenes[s].patterns[t]);
    }
  }
  w.byte(kNumSongSteps);
  uint8_t* songBits = w.bitmap(bitmapSize(kNumSongSteps));
  if (!w.ok) return 0;
  for (uint8_t s = 0; s < kNumSongSteps; ++s) {
    if (project.song[s] == kNoScene) continue;
    setBit(songBits, s);
    w.byte(project.song[s]);
  }
  return w.ok ? w.written : 0;
}

bool decodeProject(const uint8_t* data, size_t size, Project& project) {
  if (!walk(data, size, NULL)) return false;  // checked first, so bad data changes nothing
  initProject(project);
  return walk(data, size, &project);
}

}  // namespace gx
