#pragma once

#include <stdint.h>

#include "engine/EventSink.h"
#include "engine/Project.h"

namespace gx {

// The sequencer engine: owns the project data, runs the transport and clock, and
// sends what it plays to an EventSink. Knows nothing about pads, files or MIDI.
// Out-of-range track, pattern or step arguments are ignored.
// Where the beat comes from. Auto is the default: a plain master/slave switch left on slave
// with nothing sending looks like a broken instrument, so following is something that happens
// when a clock is actually there.
enum ClockSource {
  kClockInternal = 0,  // we drive, and send clock
  kClockAuto,          // follow an incoming clock while there is one, drive when there isn't
  kClockExternal,      // only ever follow: no clock, no playback
  kNumClockSources,
};

class Sequencer {
 public:
  explicit Sequencer(EventSink& output);

  // ---- Project ----
  const Project& project() const { return project_; }
  // Replaces all data and announces every track's preset.
  void setProject(const Project& project);

  // ---- Transport ----
  void play();
  void stop();
  bool playing() const { return playing_; }
  void setRecording(bool recording) { recording_ = recording; }
  bool recording() const { return recording_; }
  void setBpm(uint16_t bpm);  // clamped to kMinBpm..kMaxBpm
  uint16_t bpm() const { return project_.bpm; }
  // Swing holds every second step back, by up to half a step at kMaxSwing. It belongs to the
  // project, like the tempo, and moves when the notes play, not the grid the UI shows.
  void setSwing(uint8_t percent);  // clamped to kMinSwing..kMaxSwing
  uint8_t swing() const { return project_.swing; }
  // Step currently playing on a track. Tracks with different lengths wrap independently.
  uint16_t playhead(uint8_t track) const;
  // Advances the clock and plays due steps. Call often with a monotonic time: the more often,
  // the tighter the timing. updateMicros() takes microseconds and update() milliseconds;
  // stick to one of them.
  void update(uint32_t nowMs) { updateMicros(nowMs * 1000u); }
  void updateMicros(uint32_t nowUs);

  // ---- Clock source ----
  // Where the beat comes from. Not part of the project: it describes how this box is wired
  // into a studio, not the music, so carrying a project between rigs never re-slaves anything.
  void setClockSource(uint8_t source);  // a ClockSource
  uint8_t clockSource() const { return clockSource_; }
  // Whether the engine is being driven from outside right now. The app decides this from the
  // source and whether a clock is actually arriving; the engine only obeys it.
  void setFollowingExternal(bool following);
  bool followingExternal() const { return following_; }
  // One MIDI clock in: kTicksPerMidiClock engine ticks, since MIDI clock is 24 a quarter note
  // and the engine runs at 96. The first one after a Start plays step 1 rather than advancing,
  // which is what puts us on the master's downbeat. Ignored unless following.
  void externalClockTick(uint32_t nowUs);
  // The tempo being followed, measured from the gaps between incoming clocks, or 0 before
  // there are two to measure. The project's own bpm is left alone while following.
  uint16_t externalBpm() const;
  // Seeds the dice for step probability, so a run can be repeated.
  void seedRandom(uint32_t seed);

  // ---- Bars ----
  // Steps and bars since play(), counted globally: a track whose pattern is 12 steps long
  // wraps on its own, but the bar belongs to the song.
  uint32_t stepPosition() const { return stepCounter_; }
  uint16_t stepInBar() const { return static_cast<uint16_t>(stepCounter_ % kStepsPerBar); }
  uint32_t barPosition() const { return stepCounter_ / kStepsPerBar; }

  // ---- Patterns ----
  uint8_t selectedPattern(uint8_t track) const;
  void selectPattern(uint8_t track, uint8_t pattern);
  bool patternHasData(uint8_t track, uint8_t pattern) const;
  void clearPattern(uint8_t track, uint8_t pattern);
  void copyPattern(uint8_t fromTrack, uint8_t fromPattern, uint8_t toTrack, uint8_t toPattern);

  // ---- Steps of a track's selected pattern ----
  uint16_t trackLength(uint8_t track) const;
  // Sets how many steps the selected pattern plays, clamped to 1..kMaxSteps.
  void setTrackLength(uint8_t track, uint16_t length);
  bool stepActive(uint8_t track, uint16_t step) const;
  // Notes in a step's chord, and the index-th of them.
  uint8_t stepNoteCount(uint8_t track, uint16_t step) const;
  uint8_t stepNote(uint8_t track, uint16_t step, uint8_t index = 0) const;
  // Whether a note is in a step's chord.
  bool stepHasNote(uint8_t track, uint16_t step, uint8_t note) const;
  // A step switched on keeps its chord, or takes the track's last note if it has none.
  void toggleStep(uint8_t track, uint16_t step);
  // Replaces a step's chord with one note and switches it on.
  void setStepNote(uint8_t track, uint16_t step, uint8_t note);
  // Replaces a step's chord with these notes (up to kMaxStepNotes) and switches it on.
  void setStepChord(uint8_t track, uint16_t step, const uint8_t* notes, uint8_t count);
  // Adds a note to a step's chord, if there is room and it isn't there already.
  void addStepNote(uint8_t track, uint16_t step, uint8_t note);
  // Takes a note out of a step's chord; a step left with no notes switches off.
  void removeStepNote(uint8_t track, uint16_t step, uint8_t note);
  // Velocity of a step's notes, 1..127.
  uint8_t stepVelocity(uint8_t track, uint16_t step) const;
  void setStepVelocity(uint8_t track, uint16_t step, uint8_t velocity);
  // Chance, in percent (1..100), that an active step plays each time the playhead reaches it.
  // A chord plays whole or not at all.
  uint8_t stepProbability(uint8_t track, uint16_t step) const;
  void setStepProbability(uint8_t track, uint16_t step, uint8_t probability);
  // How long a step's notes sound, in sixths of a step, 1..kMaxGateUnits. A gate
  // longer than a step keeps its notes sounding over the steps after it.
  uint8_t stepGate(uint8_t track, uint16_t step) const;
  void setStepGate(uint8_t track, uint16_t step, uint8_t ticks);
  // Micro-timing: ticks this step plays early (negative) or late, kMinNudge..kMaxNudge. It
  // moves the notes only; the step keeps its place on the grid.
  int8_t stepNudge(uint8_t track, uint16_t step) const;
  void setStepNudge(uint8_t track, uint16_t step, int8_t ticks);
  // How many times the step fires, spread evenly across its own length: 1..kMaxRatchet. The
  // probability is rolled once, so a ratcheted step plays whole or not at all.
  uint8_t stepRatchet(uint8_t track, uint16_t step) const;
  void setStepRatchet(uint8_t track, uint16_t step, uint8_t hits);
  void clearStep(uint8_t track, uint16_t step);
  void copyStep(uint8_t track, uint16_t fromStep, uint16_t toStep);
  // While playing and recording, writes the note into the step nearest to now. A velocity of
  // 1..127 becomes the step's, so a part played on velocity-sensitive pads keeps its accents;
  // 0 means the surface could not tell and leaves the step's velocity alone.
  void recordNote(uint8_t track, uint8_t note, uint8_t velocity = 0);

  // ---- Presets ----
  uint16_t trackPreset(uint8_t track) const;
  void setTrackPreset(uint8_t track, uint16_t preset);

  // ---- Note keyboard octave, per track ----
  uint8_t keyboardOctave(uint8_t track) const;
  void setKeyboardOctave(uint8_t track, uint8_t octave);  // 0..kMaxKeyboardOctave

  // ---- Scale: which notes the note keyboard offers ----
  uint8_t scaleRoot() const { return project_.scaleRoot; }
  uint8_t scale() const { return project_.scale; }
  void setScaleRoot(uint8_t root);  // 0 = C .. 11 = B
  void setScale(uint8_t scale);     // ScaleId
  // How a track's note keyboard is laid out (KeyboardLayout): the song's scale, the track's
  // own scale, or drum pads. Each time a track switches to its own scale, it starts as a copy
  // of the song's key; switching away drops it.
  uint8_t keyboardLayout(uint8_t track) const;
  void setKeyboardLayout(uint8_t track, uint8_t layout);
  void setOwnScaleRoot(uint8_t track, uint8_t root);  // 0 = C .. 11 = B
  void setOwnScale(uint8_t track, uint8_t scale);     // ScaleId
  // The root and scale a track's note keyboard uses: its own or the song's. Drum pads use no
  // scale and count as chromatic from C.
  uint8_t keyboardScale(uint8_t track) const;
  uint8_t keyboardRoot(uint8_t track) const;
  // Whether note mode shows the track as a piano roll instead of steps and a keyboard.
  bool trackPianoRoll(uint8_t track) const;
  void setTrackPianoRoll(uint8_t track, bool on);
  // One key, one chord: how many notes a key of the track's keyboard plays (ChordShape).
  // Drum pads have no scale to build a chord on and ignore it.
  uint8_t trackChord(uint8_t track) const;
  void setTrackChord(uint8_t track, uint8_t shape);

  // ---- Arpeggiator, per track ----
  // With a mode other than kArpOff, a step plays its notes one at a time instead of together:
  // the rate says how often, the octaves how far it climbs, and the step's gate how long the
  // run lasts. A step's ratchet is ignored while the arp is on - both spread one step out, and
  // two of them at once is a muddle rather than a feature.
  uint8_t trackArpMode(uint8_t track) const;
  void setTrackArpMode(uint8_t track, uint8_t mode);
  uint8_t trackArpRate(uint8_t track) const;  // index into kArpRateTicks
  void setTrackArpRate(uint8_t track, uint8_t rate);
  uint8_t trackArpOctaves(uint8_t track) const;
  void setTrackArpOctaves(uint8_t track, uint8_t octaves);
  // The MIDI channel a track plays on, 0..15. Changing it releases the track's sounding notes
  // on the old channel and sends its preset on the new one.
  uint8_t trackMidiChannel(uint8_t track) const;
  void setTrackMidiChannel(uint8_t track, uint8_t channel);
  // The MIDI port a track plays out of, 0..kNumMidiPorts-1 (P1..P8). Changing it releases the
  // track's sounding notes on the old port and sends its preset on the new one.
  uint8_t trackMidiPort(uint8_t track) const;
  // Moves the track to a MIDI port, taking it off an internal instrument.
  void setTrackMidiPort(uint8_t track, uint8_t port);
  // The internal instrument a track plays on, or kNoInstrument when it uses its MIDI port.
  uint8_t trackInstrument(uint8_t track) const;
  bool trackUsesInstrument(uint8_t track) const;
  // Moves the track to instrument 0..kNumInstruments-1, or back to its MIDI port with
  // kNoInstrument. Like a channel change, it releases the track's notes and resends its preset.
  void setTrackInstrument(uint8_t track, uint8_t instrument);

  // ---- Mute and solo ----
  // Live state: it silences a track's sequenced notes without changing the pattern, and is
  // not saved with the project. Notes played by hand are still heard.
  bool trackMuted(uint8_t track) const;
  void setTrackMuted(uint8_t track, bool muted);
  bool trackSoloed(uint8_t track) const;
  void setTrackSoloed(uint8_t track, bool soloed);
  // Drops every solo, so everything unmuted is heard again.
  void clearSolos();
  // Whether a track's steps are heard: not muted, and soloed if any track is.
  bool trackAudible(uint8_t track) const;
  bool anySolo() const { return soloCount_ > 0; }

  // Sends a MIDI CC on a track's channel and port, for the UI's faders and knobs.
  void sendControlChange(uint8_t track, uint8_t cc, uint8_t value);

  // ---- Scenes ----
  // A scene remembers which tracks were silent, so a section can be brought back with one
  // pad. Captured from the mutes as they sound now, and saved with the project.
  bool sceneUsed(uint8_t scene) const;
  void captureScene(uint8_t scene);
  // Applies the scene's mutes. Any solo is dropped, so what plays is what the scene says.
  void launchScene(uint8_t scene);
  // Launches it at the top of the next bar, which is how a section is changed in time with
  // the music. While stopped there is no bar to wait for, so it launches at once. Queueing
  // the scene that is already waiting cancels it, and queueing another replaces it.
  void queueScene(uint8_t scene);
  // The scene waiting for the next bar, or kNoScene.
  uint8_t pendingScene() const { return pendingScene_; }
  void cancelPendingScene() { pendingScene_ = kNoScene; }
  void clearScene(uint8_t scene);
  void copyScene(uint8_t from, uint8_t to);
  // The scene launched last, or kNoScene. Live state: it isn't saved with the project.
  uint8_t currentScene() const { return currentScene_; }

  // ---- The song ----
  // Scenes in the order they play, one scene per step. Nothing plays it yet: it is written
  // and edited now, and the transport will follow it once bars exist.
  uint8_t songStep(uint8_t index) const;
  void setSongStep(uint8_t index, uint8_t scene);
  void clearSongStep(uint8_t index);
  void copySongStep(uint8_t from, uint8_t to);
  // Where the song is: the step last played or tapped, or kNoSongStep. Live, not saved.
  uint8_t songPosition() const { return songPosition_; }
  // Plays the song from a step, or from where it stands (the first filled step if nowhere):
  // each step holds its scene for kBarsPerSongStep bars, then the next filled one takes over,
  // and the song loops when it runs out. Returns false if the song is empty. While stopped it
  // only arms: the first scene lands when playback starts.
  bool startSong(uint8_t fromStep = kNoSongStep);
  // Stops following, leaving playback and the mutes as they are: the song goes on sounding,
  // it just stops changing section. Launching a scene by hand does this too.
  void stopSong() { songFollow_ = false; }
  bool followingSong() const { return songFollow_; }
  // Goes to a step and launches its scene, which is how a section is auditioned.
  void goToSongStep(uint8_t index);

  // ---- Notes played live, outside the sequence (e.g. while a note pad is held) ----
  // Plays a note for as long as a pad is held. A velocity of 0 uses the default, so a surface
  // that cannot tell how hard sounds as it always did.
  void previewNoteOn(uint8_t track, uint8_t note, uint8_t velocity = 0);
  void previewNoteOff(uint8_t track, uint8_t note);

 private:
  // Any of a track's patterns, from the project or from the built-in bank; the writable form
  // is null for the bank, which is how the bank stays read-only.
  const Pattern* patternAt(uint8_t track, uint8_t pattern) const;
  Pattern* writablePattern(uint8_t track, uint8_t pattern);
  const Pattern* currentPattern(uint8_t track) const;
  // A step of the selected pattern, named rather than overloaded on constness: as an overload
  // pair the writable one won every call inside a non-const member, whatever the variable it
  // was assigned to said, and the writable one is null for the bank - which silently stopped
  // playback reading a factory pattern while every const reader saw it perfectly well.
  const Step* stepAt(uint8_t track, uint16_t step) const;
  Step* writableStep(uint8_t track, uint16_t step);
  void advanceTick();
  // The first tick of a run: whichever clock gets there first calls it.
  void beginIfPending();
  void startNote(uint8_t track, uint8_t note, uint8_t velocity, uint32_t offTick);
  void releaseDueNotes();
  void releaseSounding(uint8_t track, uint8_t index);
  void releaseNotes();
  void releaseTrackNotes(uint8_t track);
  bool rollChance(uint8_t percent);

  EventSink& output_;
  Project project_;
  uint32_t lastUs_;
  uint32_t lastClockUs_;      // when the last MIDI clock came in
  uint32_t clockIntervalUs_;  // smoothed gap between them, for externalBpm()
  uint8_t clockSource_;
  bool following_;
  bool clockSeen_;
  uint32_t phase_;        // gains us * bpm * steps-per-beat; one tick per 10000000
  // Ticks every second step is held back by, which is what swing does.
  int16_t swingOffset(uint32_t stepIndex) const;
  // The step a track plays at a point on the grid: its pattern wraps on its own length.
  uint16_t stepAtPosition(uint8_t track, uint32_t position) const;
  // Plays whatever is due at the current tick, which is not always on the grid.
  void playDueSteps();
  void playTrackStep(uint8_t track, uint16_t step);
  // Fires the hits of a ratcheted step that have come due, and forgets the ones that are done.
  void playDueRepeats();
  void fireRepeat(uint8_t track, uint8_t hit);
  uint32_t nextRandom();

  uint32_t tick_;         // ticks since play(), kTicksPerStep a step
  uint32_t stepCounter_;  // steps since play()
  uint32_t random_;       // xorshift32 state for step probability, never 0
  // Notes playing on each track, oldest first, with the tick each one ends on.
  struct SoundingNote {
    uint8_t note;
    uint32_t offTick;
  };
  enum { kMaxSoundingNotes = 16 };
  SoundingNote sounding_[kNumTracks][kMaxSoundingNotes];
  uint8_t soundingCounts_[kNumTracks];

  // A step that fires more than once leaves the rest of its hits here, to go off later in the
  // step. A new step on that track replaces them: the next trig always wins.
  struct Repeat {
    uint32_t startTick;             // where the step's notes first sounded
    uint32_t endTick;               // arp: the last tick a note may start on
    uint8_t notes[kMaxStepNotes];   // as the step holds them
    uint8_t sorted[kMaxStepNotes];  // and in pitch order, for the arp's climbs
    uint8_t count;                  // notes in the chord
    uint8_t velocity;
    uint8_t gateUnits;              // the step's gate; a hit is never longer than its space
    uint8_t hits;                   // ratchet: 1..kMaxRatchet
    uint8_t fired;                  // how many have gone
    uint8_t arpMode;                // kArpOff for a ratchet rather than an arp
    uint8_t arpTicks;               // between arp notes
    uint8_t arpOctaves;
    bool pending;                   // something is still to fire
  };
  Repeat repeats_[kNumTracks];
  // The note the arp plays on its nth turn of a step's chord.
  uint8_t arpNote(const Repeat& repeat, uint8_t turn);
  bool muted_[kNumTracks];
  bool soloed_[kNumTracks];
  uint8_t soloCount_;  // tracks currently soloed
  uint8_t currentScene_;
  uint8_t pendingScene_;  // waiting for the top of the next bar, or kNoScene
  uint8_t songPosition_;
  bool songFollow_;        // the transport is playing the song, not just a loop
  uint32_t songStepBar_;   // bar the current song step started on
  // The next filled step after this one, wrapping; kNoSongStep if the song is empty.
  uint8_t nextSongStep(uint8_t from) const;
  // Moves the song on to a step and launches its scene.
  void enterSongStep(uint8_t index);
  bool clockStarted_;
  bool playing_;
  bool startPending_;
  bool recording_;
};

}  // namespace gx
