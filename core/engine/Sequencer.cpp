#include "engine/Sequencer.h"

namespace gx {

namespace {

const uint8_t kStepsPerBeat = 4;       // 16th notes
const uint32_t kPhasePerStep = 60000000;  // microseconds per minute
const uint32_t kPhasePerTick = kPhasePerStep / kTicksPerStep;
const uint32_t kMaxDeltaUs = 1000000;     // limits catch-up after the loop stalls
// A MIDI clock is a 24th of a quarter note, so 60e6 / 24 microseconds a minute over the gap
// between two of them is the tempo. Gaps outside the tempo range are a pause, not a tempo.
const uint32_t kUsPerMinutePerClock = 2500000;
const uint32_t kMinClockGapUs = kUsPerMinutePerClock / kMaxBpm;
const uint32_t kMaxClockGapUs = kUsPerMinutePerClock / kMinBpm;
const uint32_t kDefaultSeed = 0x6D2B79F5u;

}  // namespace

Sequencer::Sequencer(EventSink& output)
    : output_(output),
      lastUs_(0),
      lastClockUs_(0),
      clockIntervalUs_(0),
      clockSource_(kClockAuto),
      following_(false),
      clockSeen_(false),
      phase_(0),
      tick_(0),
      stepCounter_(0),
      random_(kDefaultSeed),
      soloCount_(0),
      currentScene_(kNoScene),
      pendingScene_(kNoScene),
      songPosition_(kNoSongStep),
      songFollow_(false),
      songStepBar_(0),
      clockStarted_(false),
      playing_(false),
      startPending_(false),
      recording_(false) {
  initProject(project_);
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    soundingCounts_[t] = 0;
    muted_[t] = false;
    soloed_[t] = false;
    repeats_[t].pending = false;
  }
}

// ---- Project ----

void Sequencer::setProject(const Project& project) {
  releaseNotes();
  project_ = project;
  if (project_.scaleRoot >= kNumRoots) project_.scaleRoot = 0;
  if (project_.scale >= kNumScales) project_.scale = kScaleChromatic;
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    Track& track = project_.tracks[t];
    // Guard the fields used as array indices and divisors.
    if (track.selectedPattern >= kNumPatterns) track.selectedPattern = 0;
    if (track.keyboardOctave > kMaxKeyboardOctave) track.keyboardOctave = kDefaultKeyboardOctave;
    if (track.keyboardLayout >= kNumKeyboardLayouts) track.keyboardLayout = kKeyboardProjectScale;
    if (track.scaleRoot >= kNumRoots) track.scaleRoot = 0;
    if (track.keyboardLayout != kKeyboardOwnScale || track.scale > kNoOwnScale) {
      track.scaleRoot = 0;  // only a track using its own scale keeps one
      track.scale = kNoOwnScale;
    }
    if (track.pianoRoll > 1) track.pianoRoll = 1;
    if (track.midiChannel >= kNumMidiChannels) track.midiChannel = defaultMidiChannel(t);
    if (track.midiPort >= kNumMidiPorts) track.midiPort = defaultMidiPort(t);
    if (track.instrument >= kNumInstruments) track.instrument = kNoInstrument;
    muted_[t] = false;  // mutes belong to the session, not to the project
    soloed_[t] = false;
    currentScene_ = kNoScene;
    pendingScene_ = kNoScene;
    songPosition_ = kNoSongStep;
    songFollow_ = false;
    for (uint8_t p = 0; p < kNumPatterns; ++p) {
      uint16_t& length = track.patterns[p].length;
      if (length == 0 || length > kMaxSteps) length = kMaxSteps;
    }
    output_.trackPortChanged(t, track.midiPort);        // all before the preset, which
    output_.trackChannelChanged(t, track.midiChannel);  // goes out on them
    output_.trackInstrumentChanged(t, track.instrument);
    output_.presetChanged(t, track.preset);
  }
}

// ---- Transport ----

void Sequencer::play() {
  if (playing_) return;
  playing_ = true;
  startPending_ = true;
}

void Sequencer::stop() {
  const bool wasRunning = playing_ && !startPending_;
  playing_ = false;
  startPending_ = false;
  pendingScene_ = kNoScene;  // it was waiting for a bar that will not come
  songFollow_ = false;
  if (wasRunning) output_.transportStopped();
  releaseNotes();
  tick_ = 0;
  stepCounter_ = 0;
  phase_ = 0;
}

void Sequencer::setSwing(uint8_t percent) {
  if (percent < kMinSwing) percent = kMinSwing;
  if (percent > kMaxSwing) percent = kMaxSwing;
  project_.swing = percent;
}

void Sequencer::setBpm(uint16_t bpm) {
  if (bpm < kMinBpm) bpm = kMinBpm;
  if (bpm > kMaxBpm) bpm = kMaxBpm;
  project_.bpm = bpm;
}

uint16_t Sequencer::playhead(uint8_t track) const { return stepAtPosition(track, stepCounter_); }

void Sequencer::beginIfPending() {
  if (!startPending_) return;
  startPending_ = false;
  tick_ = 0;
  stepCounter_ = 0;
  phase_ = 0;
  // Start, then the clock for tick 0: gear follows from the first step.
  output_.transportStarted();
  output_.clockTick();
  if (songFollow_ && songPosition_ != kNoSongStep) enterSongStep(songPosition_);
  playDueSteps();  // step 1 is never swung, so it sounds on the downbeat
}

void Sequencer::setClockSource(uint8_t source) {
  if (source < kNumClockSources) clockSource_ = source;
}

void Sequencer::setFollowingExternal(bool following) {
  if (following == following_) return;
  following_ = following;
  // Coming back to our own clock, the phase starts clean rather than carrying whatever was
  // left over from the tick the master last gave us.
  if (!following_) phase_ = 0;
}

uint16_t Sequencer::externalBpm() const {
  if (clockIntervalUs_ == 0) return 0;
  const uint32_t bpm = kUsPerMinutePerClock / clockIntervalUs_;
  return static_cast<uint16_t>(bpm > kMaxBpm ? kMaxBpm : bpm);
}

void Sequencer::externalClockTick(uint32_t nowUs) {
  // Measure the tempo whether or not we are following, so switching to it knows the tempo
  // already and the display has something true to show.
  if (clockSeen_) {
    const uint32_t gap = nowUs - lastClockUs_;
    if (gap >= kMinClockGapUs && gap <= kMaxClockGapUs) {
      // Smoothed over four clocks: enough to ride out jitter, short enough to follow a hand
      // on a tempo knob.
      clockIntervalUs_ = clockIntervalUs_ == 0 ? gap : (clockIntervalUs_ * 3 + gap) / 4;
    } else {
      clockIntervalUs_ = 0;  // a gap that long was a pause; start measuring again
    }
  }
  lastClockUs_ = nowUs;
  clockSeen_ = true;

  if (!following_ || !playing_) return;
  // The first clock after a Start is the downbeat, not a step forward.
  if (startPending_) {
    beginIfPending();
    return;
  }
  for (uint8_t i = 0; i < kTicksPerMidiClock; ++i) advanceTick();
}

void Sequencer::updateMicros(uint32_t nowUs) {
  // Unsigned subtraction stays correct when the microsecond counter wraps.
  uint32_t deltaUs = clockStarted_ ? nowUs - lastUs_ : 0;
  lastUs_ = nowUs;
  clockStarted_ = true;
  if (!playing_) return;
  // Following: the ticks come from externalClockTick(). Keeping our own time above means the
  // hand-over back to the internal clock starts from now, not from a stale stamp.
  if (following_) return;

  if (startPending_) {
    beginIfPending();
    deltaUs = 0;
  }

  // Integer phase accumulation gives an exact tempo with no drift, and BPM changes
  // take effect without jumping the playhead.
  // At most 1e6 us * 300 BPM * 4 = 1.2e9 per call, so the phase never overflows.
  if (deltaUs > kMaxDeltaUs) deltaUs = kMaxDeltaUs;
  phase_ += deltaUs * project_.bpm * kStepsPerBeat;
  while (phase_ >= kPhasePerTick) {
    phase_ -= kPhasePerTick;
    advanceTick();
  }
}

void Sequencer::seedRandom(uint32_t seed) { random_ = seed ? seed : kDefaultSeed; }

uint32_t Sequencer::nextRandom() {
  random_ ^= random_ << 13;
  random_ ^= random_ >> 17;
  random_ ^= random_ << 5;
  return random_;
}

bool Sequencer::rollChance(uint8_t percent) {
  if (percent >= kMaxProbability) return true;  // steps that always play leave the dice alone
  return nextRandom() % 100u < percent;
}

void Sequencer::advanceTick() {
  ++tick_;
  if (tick_ % kTicksPerMidiClock == 0) output_.clockTick();
  releaseDueNotes();  // first, so a one-step gate ends just as the next step starts
  if (tick_ % kTicksPerStep == 0) {
    ++stepCounter_;
    // The top of a bar is where a queued scene lands, before the step plays, so the first
    // step of the bar already sounds as the scene says.
    if (stepCounter_ % kStepsPerBar == 0) {
      if (pendingScene_ != kNoScene) {
        launchScene(pendingScene_);
      } else if (songFollow_ && barPosition() - songStepBar_ >= kBarsPerSongStep) {
        const uint8_t next = nextSongStep(songPosition_);
        if (next != kNoSongStep) {
          enterSongStep(next);
        } else {
          songFollow_ = false;  // every step was cleared while it played
        }
      }
    }
  }
  playDueRepeats();  // the rest of a ratcheted step, spread across it
  playDueSteps();    // on the grid, or later in the step when swing holds it back
}

// How far a step is held back from its place on the grid, in ticks. Swing delays every
// second step; the grid itself, and so the playhead the UI draws, never moves.
int16_t Sequencer::swingOffset(uint32_t stepIndex) const {
  if ((stepIndex & 1) == 0) return 0;
  const uint8_t swing = project_.swing < kMinSwing ? kMinSwing : project_.swing;
  return static_cast<int16_t>((swing - kMinSwing) * kTicksPerStep / 50);
}

uint16_t Sequencer::stepAtPosition(uint8_t track, uint32_t position) const {
  const uint16_t length = trackLength(track);
  return length ? static_cast<uint16_t>(position % length) : 0;
}

// Called every tick: a track's step sounds when the tick matches its place on the grid plus
// swing plus its own nudge. A step nudged early belongs to the tick before its place, so the
// next step is considered too.
void Sequencer::playDueSteps() {
  const int16_t within = static_cast<int16_t>(tick_ % kTicksPerStep);
  const int16_t swingNow = swingOffset(stepCounter_);
  const int16_t swingNext = swingOffset(stepCounter_ + 1);
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    const uint16_t stepNow = stepAtPosition(t, stepCounter_);
    if (within == swingNow + stepNudge(t, stepNow)) {
      playTrackStep(t, stepNow);
      continue;
    }
    const uint16_t stepNext = stepAtPosition(t, stepCounter_ + 1);
    const int16_t offsetNext = swingNext + stepNudge(t, stepNext);
    if (offsetNext < 0 && within == static_cast<int16_t>(kTicksPerStep) + offsetNext) {
      playTrackStep(t, stepNext);
    }
  }
}

void Sequencer::playTrackStep(uint8_t track, uint16_t stepIndex) {
  if (!trackAudible(track)) return;  // muted, or another track is soloed
  const Step* step = stepAt(track, stepIndex);
  if (!step || !step->active || !rollChance(step->probability)) return;
  const uint8_t count = step->noteCount < kMaxStepNotes ? step->noteCount : kMaxStepNotes;
  const uint8_t gate = step->gate > 0 ? step->gate : 1;
  Repeat& repeat = repeats_[track];
  repeat.startTick = tick_;
  repeat.count = count;
  for (uint8_t n = 0; n < count; ++n) repeat.notes[n] = step->notes[n];
  // In pitch order too, which is what the arp climbs; a chord is at most four notes, so the
  // simplest sort there is.
  for (uint8_t n = 0; n < count; ++n) repeat.sorted[n] = step->notes[n];
  for (uint8_t i = 1; i < count; ++i) {
    for (uint8_t j = i; j > 0 && repeat.sorted[j] < repeat.sorted[j - 1]; --j) {
      const uint8_t swap = repeat.sorted[j];
      repeat.sorted[j] = repeat.sorted[j - 1];
      repeat.sorted[j - 1] = swap;
    }
  }
  repeat.velocity = step->velocity;
  repeat.gateUnits = gate;
  repeat.arpMode = project_.tracks[track].arpMode;
  repeat.arpTicks = kArpRateTicks[project_.tracks[track].arpRate % kNumArpRates];
  repeat.arpOctaves = project_.tracks[track].arpOctaves;
  // The gate says how long the run lasts, so a longer gate arpeggiates over more steps.
  repeat.endTick = tick_ + static_cast<uint32_t>(gate) * kTicksPerGateUnit;
  repeat.hits = step->ratchet > 0 ? step->ratchet : 1;
  if (repeat.hits > kMaxRatchet) repeat.hits = kMaxRatchet;
  repeat.fired = 0;
  repeat.pending = true;
  fireRepeat(track, 0);  // the first hit is the step itself, on time
}

// The note an arp plays on its nth turn: the chord, in the mode's order, climbing as many
// octaves as the track asks for.
uint8_t Sequencer::arpNote(const Repeat& repeat, uint8_t turn) {
  const uint8_t count = repeat.count;
  if (count == 0) return kInvalidNote;
  const uint8_t octaves = repeat.arpOctaves > 0 ? repeat.arpOctaves : 1;
  const uint16_t total = static_cast<uint16_t>(count) * octaves;
  uint16_t index = 0;
  switch (repeat.arpMode) {
    case kArpDown:
      index = static_cast<uint16_t>(total - 1 - (turn % total));
      break;
    case kArpUpDown:
    case kArpDownUp: {
      // Up and back without playing the ends twice, so the turn takes 2n - 2 notes.
      const uint16_t period = total > 1 ? static_cast<uint16_t>(2 * total - 2) : 1;
      uint16_t at = turn % period;
      if (repeat.arpMode == kArpDownUp) at = static_cast<uint16_t>((at + total - 1) % period);
      index = at < total ? at : static_cast<uint16_t>(period - at);
      break;
    }
    case kArpRandom:
      index = static_cast<uint16_t>(nextRandom() % total);
      break;
    default:  // up, and as-played, which differ only in the order they read the chord
      index = static_cast<uint16_t>(turn % total);
      break;
  }
  const uint8_t octave = static_cast<uint8_t>(index / count);
  const uint8_t which = static_cast<uint8_t>(index % count);
  const uint8_t base = repeat.arpMode == kArpAsPlayed ? repeat.notes[which]
                                                      : repeat.sorted[which];
  const unsigned note = base + 12u * octave;
  return note <= kMaxMidiValue ? static_cast<uint8_t>(note) : kInvalidNote;
}

// Where hit n of a ratcheted step falls, counted from the step's own start: the hits are
// spread evenly, so eight of them land every third tick of the 24 a step lasts.
static uint32_t repeatOffset(uint8_t hit, uint8_t hits) {
  return static_cast<uint32_t>(hit) * kTicksPerStep / (hits > 0 ? hits : 1);
}

void Sequencer::fireRepeat(uint8_t track, uint8_t hit) {
  Repeat& repeat = repeats_[track];
  if (repeat.arpMode != kArpOff) {
    // The arp: one note of the chord, then the next one a rate later, for as long as the gate.
    const uint32_t at = repeat.startTick + static_cast<uint32_t>(hit) * repeat.arpTicks;
    uint32_t length = repeat.arpTicks;
    if (at + length > repeat.endTick) length = repeat.endTick - at;
    if (length == 0) length = 1;
    const uint8_t note = arpNote(repeat, hit);
    if (note != kInvalidNote) startNote(track, note, repeat.velocity, at + length);
    repeat.fired = static_cast<uint8_t>(hit + 1);
    const uint32_t next = repeat.startTick + static_cast<uint32_t>(repeat.fired) * repeat.arpTicks;
    if (next >= repeat.endTick || repeat.fired == 0xFF) repeat.pending = false;
    return;
  }
  const uint32_t at = repeat.startTick + repeatOffset(hit, repeat.hits);
  // A hit lasts its own gate, or the space to the next one - whichever is shorter, so a long
  // gate rolls rather than blurs.
  const uint32_t space = repeat.startTick + repeatOffset(static_cast<uint8_t>(hit + 1),
                                                         repeat.hits) - at;
  uint32_t length = static_cast<uint32_t>(repeat.gateUnits) * kTicksPerGateUnit;
  if (repeat.hits > 1 && length > space) length = space;
  if (length == 0) length = 1;
  for (uint8_t n = 0; n < repeat.count; ++n) {
    startNote(track, repeat.notes[n], repeat.velocity, at + length);
  }
  repeat.fired = static_cast<uint8_t>(hit + 1);
  if (repeat.fired >= repeat.hits) repeat.pending = false;  // done with this step
}

void Sequencer::playDueRepeats() {
  for (uint8_t track = 0; track < kNumTracks; ++track) {
    Repeat& repeat = repeats_[track];
    while (repeat.pending && repeat.count > 0) {
      const uint32_t due = repeat.arpMode != kArpOff
                               ? repeat.startTick + static_cast<uint32_t>(repeat.fired) *
                                                        repeat.arpTicks
                               : repeat.startTick + repeatOffset(repeat.fired, repeat.hits);
      if (tick_ < due) break;
      fireRepeat(track, repeat.fired);
    }
  }
}

void Sequencer::startNote(uint8_t track, uint8_t note, uint8_t velocity, uint32_t offTick) {
  // The same pitch still sounding from a long gate ends first, so it can play again; a track
  // with no room left lets its oldest note go.
  for (uint8_t i = 0; i < soundingCounts_[track]; ++i) {
    if (sounding_[track][i].note == note) {
      releaseSounding(track, i);
      break;
    }
  }
  if (soundingCounts_[track] == kMaxSoundingNotes) releaseSounding(track, 0);
  output_.noteOn(track, note, velocity);
  const SoundingNote sounding = {note, offTick};
  sounding_[track][soundingCounts_[track]++] = sounding;
}

void Sequencer::releaseSounding(uint8_t track, uint8_t index) {
  output_.noteOff(track, sounding_[track][index].note);
  for (uint8_t i = index; i + 1 < soundingCounts_[track]; ++i) {
    sounding_[track][i] = sounding_[track][i + 1];
  }
  --soundingCounts_[track];
}

void Sequencer::releaseDueNotes() {
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    for (uint8_t i = 0; i < soundingCounts_[t];) {
      if (sounding_[t][i].offTick <= tick_) {
        releaseSounding(t, i);
      } else {
        ++i;
      }
    }
  }
}

void Sequencer::releaseNotes() {
  for (uint8_t t = 0; t < kNumTracks; ++t) releaseTrackNotes(t);
}

bool Sequencer::trackMuted(uint8_t track) const { return track < kNumTracks && muted_[track]; }

bool Sequencer::trackSoloed(uint8_t track) const { return track < kNumTracks && soloed_[track]; }

bool Sequencer::trackAudible(uint8_t track) const {
  if (track >= kNumTracks || muted_[track]) return false;
  return soloCount_ == 0 || soloed_[track];
}

void Sequencer::setTrackMuted(uint8_t track, bool muted) {
  if (track >= kNumTracks || muted_[track] == muted) return;
  muted_[track] = muted;
  if (!trackAudible(track)) releaseTrackNotes(track);  // what it is playing stops now
}

void Sequencer::setTrackSoloed(uint8_t track, bool soloed) {
  if (track >= kNumTracks || soloed_[track] == soloed) return;
  soloed_[track] = soloed;
  soloCount_ = static_cast<uint8_t>(soloed ? soloCount_ + 1 : soloCount_ - 1);
  // The first solo silences every other track; dropping the last one brings them back.
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    if (!trackAudible(t)) releaseTrackNotes(t);
  }
}

void Sequencer::releaseTrackNotes(uint8_t track) {
  for (uint8_t n = 0; n < soundingCounts_[track]; ++n) {
    output_.noteOff(track, sounding_[track][n].note);
  }
  soundingCounts_[track] = 0;
  repeats_[track].pending = false;  // what was still to fire belongs to a step that has gone
}

// ---- Patterns ----

uint8_t Sequencer::selectedPattern(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].selectedPattern : 0;
}

void Sequencer::selectPattern(uint8_t track, uint8_t pattern) {
  if (track >= kNumTracks || pattern >= kNumPatterns) return;
  project_.tracks[track].selectedPattern = pattern;
}

bool Sequencer::patternHasData(uint8_t track, uint8_t pattern) const {
  if (track >= kNumTracks || pattern >= kNumPatterns) return false;
  return !isPatternEmpty(project_.tracks[track].patterns[pattern]);
}

void Sequencer::clearPattern(uint8_t track, uint8_t pattern) {
  if (track >= kNumTracks || pattern >= kNumPatterns) return;
  initPattern(project_.tracks[track].patterns[pattern]);
}

void Sequencer::copyPattern(uint8_t fromTrack, uint8_t fromPattern, uint8_t toTrack,
                            uint8_t toPattern) {
  if (fromTrack >= kNumTracks || toTrack >= kNumTracks || fromPattern >= kNumPatterns ||
      toPattern >= kNumPatterns) {
    return;
  }
  project_.tracks[toTrack].patterns[toPattern] = project_.tracks[fromTrack].patterns[fromPattern];
}

// ---- Steps ----

const Pattern* Sequencer::currentPattern(uint8_t track) const {
  if (track >= kNumTracks) return nullptr;
  const Track& t = project_.tracks[track];
  return &t.patterns[t.selectedPattern];
}

const Step* Sequencer::stepAt(uint8_t track, uint16_t step) const {
  const Pattern* pattern = currentPattern(track);
  return (pattern && step < kMaxSteps) ? &pattern->steps[step] : nullptr;
}

Step* Sequencer::stepAt(uint8_t track, uint16_t step) {
  return const_cast<Step*>(static_cast<const Sequencer*>(this)->stepAt(track, step));
}

uint16_t Sequencer::trackLength(uint8_t track) const {
  const Pattern* pattern = currentPattern(track);
  return pattern ? pattern->length : 0;
}

void Sequencer::setTrackLength(uint8_t track, uint16_t length) {
  if (track >= kNumTracks || length < 1) return;
  Track& t = project_.tracks[track];
  t.patterns[t.selectedPattern].length = length > kMaxSteps ? kMaxSteps : length;
}

bool Sequencer::stepActive(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s && s->active;
}

uint8_t Sequencer::stepNoteCount(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->noteCount : 0;
}

uint8_t Sequencer::stepNote(uint8_t track, uint16_t step, uint8_t index) const {
  const Step* s = stepAt(track, step);
  return (s && index < s->noteCount) ? s->notes[index] : 0;
}

bool Sequencer::stepHasNote(uint8_t track, uint16_t step, uint8_t note) const {
  const Step* s = stepAt(track, step);
  if (!s) return false;
  for (uint8_t n = 0; n < s->noteCount && n < kMaxStepNotes; ++n) {
    if (s->notes[n] == note) return true;
  }
  return false;
}

void Sequencer::toggleStep(uint8_t track, uint16_t step) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (s->active) {
    s->active = 0;
    return;  // the chord is kept, so switching it back on restores it
  }
  s->active = 1;
  if (s->noteCount == 0) {
    s->notes[0] = project_.tracks[track].note;
    s->noteCount = 1;
  }
}

void Sequencer::setStepNote(uint8_t track, uint16_t step, uint8_t note) {
  setStepChord(track, step, &note, 1);
}

void Sequencer::setStepChord(uint8_t track, uint16_t step, const uint8_t* notes, uint8_t count) {
  Step* s = stepAt(track, step);
  if (!s || notes == nullptr) return;
  if (count > kMaxStepNotes) count = kMaxStepNotes;
  uint8_t written = 0;
  for (uint8_t n = 0; n < count; ++n) {
    if (notes[n] <= kMaxMidiValue) s->notes[written++] = notes[n];
  }
  if (written == 0) return;
  for (uint8_t n = written; n < kMaxStepNotes; ++n) s->notes[n] = 0;
  s->noteCount = written;
  s->active = 1;
  project_.tracks[track].note = s->notes[written - 1];
}

void Sequencer::addStepNote(uint8_t track, uint16_t step, uint8_t note) {
  Step* s = stepAt(track, step);
  if (!s || note > kMaxMidiValue) return;
  for (uint8_t n = 0; n < s->noteCount; ++n) {
    if (s->notes[n] == note) {
      s->active = 1;
      return;  // already in the chord
    }
  }
  if (s->noteCount >= kMaxStepNotes) return;  // the chord is full
  s->notes[s->noteCount++] = note;
  s->active = 1;
  project_.tracks[track].note = note;
}

void Sequencer::removeStepNote(uint8_t track, uint16_t step, uint8_t note) {
  Step* s = stepAt(track, step);
  if (!s) return;
  uint8_t kept = 0;
  for (uint8_t n = 0; n < s->noteCount && n < kMaxStepNotes; ++n) {
    if (s->notes[n] != note) s->notes[kept++] = s->notes[n];
  }
  for (uint8_t n = kept; n < kMaxStepNotes; ++n) s->notes[n] = 0;
  s->noteCount = kept;
  if (kept == 0) s->active = 0;
}

uint8_t Sequencer::stepVelocity(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->velocity : kDefaultVelocity;
}

void Sequencer::setStepVelocity(uint8_t track, uint16_t step, uint8_t velocity) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (velocity < 1) velocity = 1;  // velocity 0 means note-off in MIDI
  if (velocity > kMaxMidiValue) velocity = kMaxMidiValue;
  s->velocity = velocity;
}

uint8_t Sequencer::stepProbability(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->probability : kMaxProbability;
}

void Sequencer::setStepProbability(uint8_t track, uint16_t step, uint8_t probability) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (probability < 1) probability = 1;
  if (probability > kMaxProbability) probability = kMaxProbability;
  s->probability = probability;
}

int8_t Sequencer::stepNudge(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->nudge : kDefaultNudge;
}

uint8_t Sequencer::stepRatchet(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->ratchet : kDefaultRatchet;
}

void Sequencer::setStepRatchet(uint8_t track, uint16_t step, uint8_t hits) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (hits < 1) hits = 1;
  if (hits > kMaxRatchet) hits = kMaxRatchet;
  s->ratchet = hits;
}

void Sequencer::setStepNudge(uint8_t track, uint16_t step, int8_t ticks) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (ticks < kMinNudge) ticks = kMinNudge;
  if (ticks > kMaxNudge) ticks = kMaxNudge;
  s->nudge = ticks;
}

uint8_t Sequencer::stepGate(uint8_t track, uint16_t step) const {
  const Step* s = stepAt(track, step);
  return s ? s->gate : kDefaultGateUnits;
}

void Sequencer::setStepGate(uint8_t track, uint16_t step, uint8_t ticks) {
  Step* s = stepAt(track, step);
  if (!s) return;
  if (ticks < 1) ticks = 1;
  if (ticks > kMaxGateUnits) ticks = kMaxGateUnits;
  s->gate = ticks;
}

void Sequencer::clearStep(uint8_t track, uint16_t step) {
  Step* s = stepAt(track, step);
  if (!s) return;
  s->active = 0;
  s->noteCount = 0;
  for (uint8_t n = 0; n < kMaxStepNotes; ++n) s->notes[n] = 0;
  s->velocity = kDefaultVelocity;
  s->probability = kMaxProbability;
  s->gate = kDefaultGateUnits;
}

void Sequencer::copyStep(uint8_t track, uint16_t fromStep, uint16_t toStep) {
  const Step* from = stepAt(track, fromStep);
  Step* to = stepAt(track, toStep);
  if (from && to) *to = *from;
}

void Sequencer::recordNote(uint8_t track, uint8_t note, uint8_t velocity) {
  if (!playing_ || !recording_) return;
  const uint16_t length = trackLength(track);
  if (length == 0) return;
  uint16_t step = playhead(track);
  // Quantise: a note played in the second half of a step belongs to the next one.
  const uint32_t intoStep = (tick_ % kTicksPerStep) * kPhasePerTick + phase_;
  if (!startPending_ && intoStep >= kPhasePerStep / 2) {
    step = static_cast<uint16_t>((step + 1) % length);
  }
  addStepNote(track, step, note);  // notes played into one step build a chord
  // The hit sets the step's loudness: velocity belongs to the step, so the last note played
  // into it speaks for the chord. A surface with nothing to say leaves it as it was.
  if (velocity > 0) setStepVelocity(track, step, velocity);
}

// ---- Presets ----

uint16_t Sequencer::trackPreset(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].preset : 0;
}

void Sequencer::setTrackPreset(uint8_t track, uint16_t preset) {
  if (track >= kNumTracks) return;
  project_.tracks[track].preset = preset;
  output_.presetChanged(track, preset);
}

// ---- Note keyboard octave ----

uint8_t Sequencer::keyboardOctave(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].keyboardOctave : kDefaultKeyboardOctave;
}

void Sequencer::setKeyboardOctave(uint8_t track, uint8_t octave) {
  if (track >= kNumTracks || octave > kMaxKeyboardOctave) return;
  project_.tracks[track].keyboardOctave = octave;
}

// ---- Scale ----

void Sequencer::setScaleRoot(uint8_t root) {
  if (root < kNumRoots) project_.scaleRoot = root;
}

void Sequencer::setScale(uint8_t scale) {
  if (scale < kNumScales) project_.scale = scale;
}

uint8_t Sequencer::trackChord(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].chord : static_cast<uint8_t>(kChordOff);
}

// Drum pads have no scale to build a chord on and the piano roll has no keyboard to play one
// from, so a track plays chords, drum pads or a piano roll - never two of them. A track's own
// scale is not in that group: a chord is built from whatever scale the track uses.
void Sequencer::setTrackChord(uint8_t track, uint8_t shape) {
  if (track >= kNumTracks) return;
  if (shape != kChordOff && shape != kChordTriad && shape != kChordSeventh) return;
  Track& t = project_.tracks[track];
  t.chord = shape;
  if (shape == kChordOff) return;
  t.pianoRoll = 0;
  if (t.keyboardLayout == kKeyboardDrums) setKeyboardLayout(track, kKeyboardProjectScale);
}

uint8_t Sequencer::trackArpMode(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].arpMode : static_cast<uint8_t>(kArpOff);
}

void Sequencer::setTrackArpMode(uint8_t track, uint8_t mode) {
  if (track >= kNumTracks || mode >= kNumArpModes) return;
  project_.tracks[track].arpMode = mode;
}

uint8_t Sequencer::trackArpRate(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].arpRate : kDefaultArpRate;
}

void Sequencer::setTrackArpRate(uint8_t track, uint8_t rate) {
  if (track >= kNumTracks || rate >= kNumArpRates) return;
  project_.tracks[track].arpRate = rate;
}

uint8_t Sequencer::trackArpOctaves(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].arpOctaves : kDefaultArpOctaves;
}

void Sequencer::setTrackArpOctaves(uint8_t track, uint8_t octaves) {
  if (track >= kNumTracks || octaves < 1 || octaves > kMaxArpOctaves) return;
  project_.tracks[track].arpOctaves = octaves;
}

uint8_t Sequencer::keyboardLayout(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].keyboardLayout
                            : static_cast<uint8_t>(kKeyboardProjectScale);
}

void Sequencer::setKeyboardLayout(uint8_t track, uint8_t layout) {
  if (track >= kNumTracks || layout >= kNumKeyboardLayouts) return;
  Track& t = project_.tracks[track];
  if (layout == t.keyboardLayout) return;
  if (layout == kKeyboardOwnScale) {
    t.scaleRoot = project_.scaleRoot;  // an own scale starts from the song's key
    t.scale = project_.scale;
  } else {
    t.scaleRoot = 0;  // and is dropped when the track leaves it
    t.scale = kNoOwnScale;
  }
  t.keyboardLayout = layout;
  // Drum pads are consecutive notes, with no scale to stack a chord on.
  if (layout == kKeyboardDrums) t.chord = kChordOff;
}

void Sequencer::setOwnScaleRoot(uint8_t track, uint8_t root) {
  if (track < kNumTracks && root < kNumRoots) project_.tracks[track].scaleRoot = root;
}

void Sequencer::setOwnScale(uint8_t track, uint8_t scale) {
  if (track < kNumTracks && scale < kNumScales) project_.tracks[track].scale = scale;
}

uint8_t Sequencer::keyboardScale(uint8_t track) const {
  switch (keyboardLayout(track)) {
    case kKeyboardOwnScale: {
      const uint8_t scale = project_.tracks[track].scale;
      return scale < kNumScales ? scale : project_.scale;
    }
    case kKeyboardDrums:
      return kScaleChromatic;
    default:
      return project_.scale;
  }
}

uint8_t Sequencer::keyboardRoot(uint8_t track) const {
  switch (keyboardLayout(track)) {
    case kKeyboardOwnScale: {
      const Track& t = project_.tracks[track];
      return t.scale < kNumScales ? t.scaleRoot : project_.scaleRoot;
    }
    case kKeyboardDrums:
      return 0;
    default:
      return project_.scaleRoot;
  }
}

bool Sequencer::trackPianoRoll(uint8_t track) const {
  return track < kNumTracks && project_.tracks[track].pianoRoll;
}

void Sequencer::setTrackPianoRoll(uint8_t track, bool on) {
  if (track >= kNumTracks) return;
  project_.tracks[track].pianoRoll = on ? 1 : 0;
  // The roll has no keyboard, so it has nowhere to play a chord from.
  if (on) project_.tracks[track].chord = kChordOff;
}

uint8_t Sequencer::trackMidiChannel(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].midiChannel : defaultMidiChannel(track);
}

void Sequencer::setTrackMidiChannel(uint8_t track, uint8_t channel) {
  if (track >= kNumTracks || channel >= kNumMidiChannels) return;
  Track& t = project_.tracks[track];
  if (t.midiChannel == channel) return;
  releaseTrackNotes(track);  // the note-offs still go to the old channel
  t.midiChannel = channel;
  output_.trackChannelChanged(track, channel);
  output_.presetChanged(track, t.preset);  // the instrument on the new channel gets the sound
}

uint8_t Sequencer::trackMidiPort(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].midiPort : defaultMidiPort(track);
}

void Sequencer::setTrackMidiPort(uint8_t track, uint8_t port) {
  if (track >= kNumTracks || port >= kNumMidiPorts) return;
  Track& t = project_.tracks[track];
  const bool leavingInstrument = t.instrument != kNoInstrument;
  if (t.midiPort == port && !leavingInstrument) return;
  releaseTrackNotes(track);  // the note-offs still go where the track was
  t.midiPort = port;
  if (leavingInstrument) {
    t.instrument = kNoInstrument;
    output_.trackInstrumentChanged(track, kNoInstrument);
  }
  output_.trackPortChanged(track, port);
  output_.presetChanged(track, t.preset);  // whatever is on the new port gets the sound
}

uint8_t Sequencer::trackInstrument(uint8_t track) const {
  return track < kNumTracks ? project_.tracks[track].instrument : kNoInstrument;
}

bool Sequencer::trackUsesInstrument(uint8_t track) const {
  return trackInstrument(track) != kNoInstrument;
}

void Sequencer::setTrackInstrument(uint8_t track, uint8_t instrument) {
  if (track >= kNumTracks) return;
  if (instrument >= kNumInstruments && instrument != kNoInstrument) return;
  Track& t = project_.tracks[track];
  if (t.instrument == instrument) return;
  releaseTrackNotes(track);  // the note-offs still go where the track was
  t.instrument = instrument;
  output_.trackInstrumentChanged(track, instrument);
  output_.presetChanged(track, t.preset);
}

void Sequencer::sendControlChange(uint8_t track, uint8_t cc, uint8_t value) {
  if (track >= kNumTracks || cc > kMaxMidiValue || value > kMaxMidiValue) return;
  output_.controlChange(track, cc, value);
}

// ---- Scenes ----

bool Sequencer::sceneUsed(uint8_t scene) const {
  return scene < kNumScenes && project_.scenes[scene].used != 0;
}

void Sequencer::captureScene(uint8_t scene) {
  if (scene >= kNumScenes) return;
  uint32_t muted = 0;
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    if (muted_[t]) muted |= 1u << t;
  }
  project_.scenes[scene].used = 1;
  project_.scenes[scene].muted = muted;
  // What is playing, not only what is silent: a section is its patterns too.
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    project_.scenes[scene].patterns[t] = project_.tracks[t].selectedPattern;
  }
  currentScene_ = scene;  // what you just captured is what you are hearing
}

void Sequencer::queueScene(uint8_t scene) {
  if (!sceneUsed(scene)) return;
  songFollow_ = false;  // picking a section by hand takes over from the song
  if (!playing_) {          // no bar to wait for
    launchScene(scene);
    return;
  }
  // Tapping the scene that is already waiting is how you change your mind.
  pendingScene_ = pendingScene_ == scene ? kNoScene : scene;
}

void Sequencer::clearSolos() {
  for (uint8_t t = 0; t < kNumTracks; ++t) setTrackSoloed(t, false);
}

void Sequencer::launchScene(uint8_t scene) {
  if (!sceneUsed(scene)) return;
  pendingScene_ = kNoScene;  // an immediate launch overrides whatever was waiting
  const uint32_t muted = project_.scenes[scene].muted;
  for (uint8_t t = 0; t < kNumTracks; ++t) {
    setTrackSoloed(t, false);  // a solo would mask the scene and look like a fault
    setTrackMuted(t, (muted >> t) & 1);
    // Scenes from before patterns were remembered leave them alone.
    const uint8_t pattern = project_.scenes[scene].patterns[t];
    if (pattern != kNoPattern) selectPattern(t, pattern);
  }
  currentScene_ = scene;
}

void Sequencer::clearScene(uint8_t scene) {
  if (scene >= kNumScenes) return;
  project_.scenes[scene].used = 0;
  project_.scenes[scene].muted = 0;
  for (uint8_t t = 0; t < kNumTracks; ++t) project_.scenes[scene].patterns[t] = kNoPattern;
  if (currentScene_ == scene) currentScene_ = kNoScene;
  if (pendingScene_ == scene) pendingScene_ = kNoScene;
}

void Sequencer::copyScene(uint8_t from, uint8_t to) {
  if (from >= kNumScenes || to >= kNumScenes || from == to) return;
  project_.scenes[to] = project_.scenes[from];
}

// ---- The song ----

uint8_t Sequencer::songStep(uint8_t index) const {
  return index < kNumSongSteps ? project_.song[index] : kNoScene;
}

void Sequencer::setSongStep(uint8_t index, uint8_t scene) {
  if (index >= kNumSongSteps) return;
  if (scene != kNoScene && !sceneUsed(scene)) return;  // an empty scene would play nothing
  project_.song[index] = scene;
}

void Sequencer::clearSongStep(uint8_t index) {
  if (index >= kNumSongSteps) return;
  project_.song[index] = kNoScene;  // a hole, which playback will skip
  if (songPosition_ == index) songPosition_ = kNoSongStep;
}

void Sequencer::copySongStep(uint8_t from, uint8_t to) {
  if (from >= kNumSongSteps || to >= kNumSongSteps || from == to) return;
  project_.song[to] = project_.song[from];
}

void Sequencer::goToSongStep(uint8_t index) {
  const uint8_t scene = songStep(index);
  if (scene == kNoScene) return;
  enterSongStep(index);
}

// Moves to a step and plays its scene. The step gets its full length from here, so jumping
// about while the song plays never lands you half way through a section.
void Sequencer::enterSongStep(uint8_t index) {
  songPosition_ = index;
  songStepBar_ = barPosition();
  launchScene(songStep(index));
}

uint8_t Sequencer::nextSongStep(uint8_t from) const {
  for (uint8_t i = 1; i <= kNumSongSteps; ++i) {
    const uint8_t index = static_cast<uint8_t>((from + i) % kNumSongSteps);
    if (songStep(index) != kNoScene) return index;  // holes are skipped; the song loops
  }
  return kNoSongStep;
}

bool Sequencer::startSong(uint8_t fromStep) {
  uint8_t step = fromStep;
  if (step >= kNumSongSteps || songStep(step) == kNoScene) step = songPosition_;
  if (step >= kNumSongSteps || songStep(step) == kNoScene) {
    step = songStep(0) != kNoScene ? 0 : nextSongStep(0);
  }
  if (step == kNoSongStep) return false;  // an empty song has nothing to play

  songFollow_ = true;
  if (playing_ && !startPending_) {
    enterSongStep(step);
  } else {
    songPosition_ = step;  // its scene lands when playback starts
  }
  return true;
}

// ---- Live notes ----

void Sequencer::previewNoteOn(uint8_t track, uint8_t note, uint8_t velocity) {
  if (track >= kNumTracks || note > kMaxMidiValue) return;
  output_.noteOn(track, note, velocity > 0 ? velocity : kDefaultVelocity);
}

void Sequencer::previewNoteOff(uint8_t track, uint8_t note) {
  if (track < kNumTracks && note <= kMaxMidiValue) output_.noteOff(track, note);
}

}  // namespace gx
