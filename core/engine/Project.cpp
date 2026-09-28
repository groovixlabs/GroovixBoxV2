#include "engine/Project.h"

namespace gx {

namespace {

// General MIDI drum notes, repeated for tracks beyond 16: kick, snare, closed hat, open hat,
// clap, low tom, crash, ride, side stick, electric snare, low floor tom, high floor tom,
// low-mid tom, hi-mid tom, high tom, cowbell.
const uint8_t kDefaultNotes[] = {36, 38, 42, 46, 39, 45, 49, 51,
                                 37, 40, 41, 43, 47, 48, 50, 56};
const uint8_t kNumDefaultNotes = sizeof(kDefaultNotes) / sizeof(kDefaultNotes[0]);

}  // namespace

uint8_t defaultTrackNote(uint8_t track) { return kDefaultNotes[track % kNumDefaultNotes]; }

bool isStepDefault(const Step& step) {
  if (step.active || step.noteCount != 0) return false;
  for (uint8_t n = 0; n < kMaxStepNotes; ++n) {
    if (step.notes[n] != 0) return false;
  }
  return step.velocity == kDefaultVelocity && step.probability == kMaxProbability &&
         step.gate == kDefaultGateUnits && step.nudge == kDefaultNudge &&
         step.ratchet == kDefaultRatchet;
}

bool isPatternDefault(const Pattern& pattern) {
  if (pattern.length != kDefaultPatternLength) return false;
  for (uint16_t s = 0; s < kMaxSteps; ++s) {
    if (!isStepDefault(pattern.steps[s])) return false;
  }
  return true;
}

bool isTrackHeaderDefault(const Track& track, uint8_t index) {
  return track.selectedPattern == 0 && track.preset == 0 &&
         track.note == defaultTrackNote(index) &&
         track.keyboardOctave == kDefaultKeyboardOctave &&
         track.keyboardLayout == kKeyboardProjectScale && track.scaleRoot == 0 &&
         track.scale == kNoOwnScale && track.pianoRoll == 0 && track.chord == kChordOff &&
         track.arpMode == kArpOff && track.arpRate == kDefaultArpRate &&
         track.arpOctaves == kDefaultArpOctaves &&
         track.midiChannel == defaultMidiChannel(index) &&
         track.midiPort == defaultMidiPort(index) && track.instrument == kNoInstrument;
}

void initPattern(Pattern& pattern) {
  for (uint16_t s = 0; s < kMaxSteps; ++s) {
    Step& step = pattern.steps[s];
    step.active = 0;
    step.noteCount = 0;  // switching it on gives it the track's last note
    for (uint8_t n = 0; n < kMaxStepNotes; ++n) step.notes[n] = 0;
    step.velocity = kDefaultVelocity;
    step.probability = kMaxProbability;
    step.gate = kDefaultGateUnits;
    step.nudge = kDefaultNudge;
    step.ratchet = kDefaultRatchet;
  }
  pattern.length = kDefaultPatternLength;
}

void initProject(Project& project) {
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    Track& track = project.tracks[t];
    const uint8_t note = defaultTrackNote(t);
    for (uint8_t p = 0; p < kNumPatterns; ++p) initPattern(track.patterns[p]);
    track.selectedPattern = 0;
    track.preset = 0;
    track.note = note;
    track.keyboardOctave = kDefaultKeyboardOctave;
    track.keyboardLayout = kKeyboardProjectScale;
    track.scaleRoot = 0;
    track.scale = kNoOwnScale;
    track.pianoRoll = 0;
    track.chord = kChordOff;
    track.arpMode = kArpOff;
    track.arpRate = kDefaultArpRate;
    track.arpOctaves = kDefaultArpOctaves;
    track.midiChannel = defaultMidiChannel(t);
    track.midiPort = defaultMidiPort(t);
    track.instrument = kNoInstrument;
  }
  for (uint8_t s = 0; s < kNumScenes; ++s) {
    project.scenes[s].used = 0;
    project.scenes[s].muted = 0;
    for (uint8_t t = 0; t < kNumTracks; ++t) project.scenes[s].patterns[t] = kNoPattern;
  }
  for (uint8_t s = 0; s < kNumSongSteps; ++s) project.song[s] = kNoScene;
  project.bpm = kDefaultBpm;
  project.swing = kDefaultSwing;
  project.scaleRoot = 0;
  project.scale = kScaleChromatic;
}

bool isPatternEmpty(const Pattern& pattern) {
  for (uint16_t s = 0; s < kMaxSteps; ++s) {
    if (pattern.steps[s].active) return false;
  }
  return true;
}

bool isProjectEmpty(const Project& project) {
  if (project.bpm != kDefaultBpm || project.swing != kDefaultSwing || project.scaleRoot != 0 ||
      project.scale != kScaleChromatic) {
    return false;
  }
  for (uint8_t s = 0; s < kNumScenes; ++s) {
    if (project.scenes[s].used) return false;
  }
  for (uint8_t s = 0; s < kNumSongSteps; ++s) {
    if (project.song[s] != kNoScene) return false;
  }
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    const Track& track = project.tracks[t];
    // The same question the codec asks before it writes a track, asked once: a second list of
    // what counts as untouched would drift from it, and did.
    if (!isTrackHeaderDefault(track, t)) return false;
    for (uint8_t p = 0; p < kNumPatterns; ++p) {
      if (!isPatternEmpty(track.patterns[p])) return false;
    }
  }
  return true;
}

}  // namespace gx
