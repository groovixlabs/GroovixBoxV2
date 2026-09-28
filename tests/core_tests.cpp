// Host tests for the portable core. Every platform interface is replaced by an in-memory
// fake, which is also a check that the interfaces really are interchangeable.
//
//   gx_tests [SCRATCH_DIR]   (SCRATCH_DIR is used for the FileStorage test)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <initializer_list>

#include "common/ApcMiniSurface.h"

#include "app/GroovixApp.h"
#include "comm/MidiParser.h"
#ifdef GX_HAVE_AUDIO  // the instrument engine is a desktop/single-board build only
#include "audio/AudioEngine.h"
#include "audio/InstrumentConfig.h"
#include "audio/NullBackend.h"
#include "audio/SpscQueue.h"
#include "audio/WavWriter.h"
#endif
#include "comm/InstrumentOutput.h"
#include "comm/MidiEventSink.h"
#include "common/FileStorage.h"
#include "engine/Sequencer.h"
#include "storage/ProjectCodec.h"
#include "storage/SlotStore.h"
#include "ui/NoteMode.h"
#include "ui/Palette.h"

namespace {

int gFailures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      ++gFailures;                                                             \
    }                                                                          \
  } while (0)

// ---- Fakes ----

enum EventType { kNoteOn, kNoteOff, kPreset, kChannel, kPort, kControl, kInstrument };

struct SinkEvent {
  EventType type;
  uint8_t track;
  uint16_t value;    // note or preset
  uint8_t velocity;  // note-ons only
};

class RecordingSink : public gx::EventSink {
 public:
  void noteOn(uint8_t track, uint8_t note, uint8_t velocity) override {
    add(kNoteOn, track, note, velocity);
  }
  void noteOff(uint8_t track, uint8_t note) override { add(kNoteOff, track, note, 0); }
  void presetChanged(uint8_t track, uint16_t preset) override { add(kPreset, track, preset, 0); }
  void trackChannelChanged(uint8_t track, uint8_t channel) override {
    add(kChannel, track, channel, 0);
  }
  void trackPortChanged(uint8_t track, uint8_t port) override { add(kPort, track, port, 0); }
  void trackInstrumentChanged(uint8_t track, uint8_t instrument) override {
    add(kInstrument, track, instrument, 0);
  }
  // CC events keep the CC number in value and the CC's value in velocity.
  void controlChange(uint8_t track, uint8_t cc, uint8_t value) override {
    add(kControl, track, cc, value);
  }
  // Counted rather than listed: a clock every tick would swamp the event list.
  void clockTick() override { ++clocks; }
  void transportStarted() override {
    ++starts;
    clocksAtStart = clocks;  // 0 when Start came before the first clock, as it must
  }
  void transportStopped() override { ++stops; }

  bool contains(EventType type, uint8_t track, uint16_t value) const {
    for (size_t i = 0; i < events.size(); ++i) {
      if (is(i, type, track, value)) return true;
    }
    return false;
  }
  bool is(size_t i, EventType type, uint8_t track, uint16_t value) const {
    return i < events.size() && events[i].type == type && events[i].track == track &&
           events[i].value == value;
  }

  std::vector<SinkEvent> events;
  unsigned clocks = 0;
  unsigned starts = 0;
  unsigned stops = 0;
  unsigned clocksAtStart = 0;

 private:
  void add(EventType type, uint8_t track, uint16_t value, uint8_t velocity) {
    SinkEvent e = {type, track, value, velocity};
    events.push_back(e);
  }
};

// Collects what tracks on internal instruments play, as text so it reads in the checks.
class RecordingInstruments : public gx::InstrumentOutput {
 public:
  void noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) override {
    add("on", instrument, note, velocity);
  }
  void noteOff(uint8_t instrument, uint8_t note) override { add("off", instrument, note, -1); }
  void controlChange(uint8_t instrument, uint8_t cc, uint8_t value) override {
    add("cc", instrument, cc, value);
  }
  void presetChanged(uint8_t instrument, uint16_t preset) override {
    add("preset", instrument, static_cast<int>(preset), -1);
  }

  std::vector<std::string> events;

 private:
  void add(const char* what, uint8_t instrument, int a, int b) {
    char line[64];
    if (b < 0) {
      std::snprintf(line, sizeof(line), "%s %d %d", what, instrument, a);
    } else {
      std::snprintf(line, sizeof(line), "%s %d %d %d", what, instrument, a, b);
    }
    events.push_back(line);
  }
};

class RecordingMidi : public gx::MidiOutput {
 public:
  void send(const gx::MidiMessage& message) override { sent.push_back(message); }
  std::vector<gx::MidiMessage> sent;
};

class MemoryStorage : public gx::Storage {
 public:
  bool read(const char* key, uint8_t* buffer, size_t capacity, size_t& size) override {
    auto it = blobs.find(key);
    if (it == blobs.end() || it->second.size() > capacity) return false;
    size = it->second.size();
    if (size) std::memcpy(buffer, &it->second[0], size);
    return true;
  }
  bool write(const char* key, const uint8_t* data, size_t size) override {
    blobs[key].assign(data, data + size);
    return true;
  }
  bool exists(const char* key) override { return blobs.count(key) != 0; }
  bool remove(const char* key) override {
    blobs.erase(key);
    return true;
  }
  std::map<std::string, std::vector<uint8_t> > blobs;
};

class FakeSurface : public gx::ControlSurface {
 public:
  FakeSurface() { frame.clear(); }
  bool pollEvent(gx::ControlEvent& event) override {
    if (queue.empty()) return false;
    event = queue.front();
    queue.erase(queue.begin());
    return true;
  }
  void show(const gx::LedFrame& shown) override { frame = shown; }

  void press(uint8_t group, uint8_t index) { push(group, index, true); }
  // A pad hit as hard as a velocity-sensitive surface would report.
  void hitPad(uint8_t pad, uint8_t velocity) {
    push(gx::kGroupPad, pad, true, velocity);
    push(gx::kGroupPad, pad, false);
  }
  void release(uint8_t group, uint8_t index) { push(group, index, false); }
  void tap(uint8_t group, uint8_t index) {
    press(group, index);
    release(group, index);
  }
  void tapPad(uint8_t pad) { tap(gx::kGroupPad, pad); }
  void tapButton(uint8_t button) { tap(gx::kGroupRight, button); }
  void moveFader(uint8_t group, uint8_t index, uint16_t value) {
    gx::ControlEvent e = {group, index, false, value, false, 0};
    queue.push_back(e);
  }
  // A position reported at start-up, as hardware does when it is scanned.
  void reportFader(uint8_t group, uint8_t index, uint16_t value) {
    gx::ControlEvent e = {group, index, false, value, true, 0};
    queue.push_back(e);
  }
  // Holds a right-hand button while tapping a track button.
  void holdAndTapTrack(uint8_t button, uint8_t track) {
    press(gx::kGroupRight, button);
    tap(gx::kGroupBottom, track);
    release(gx::kGroupRight, button);
  }

  std::vector<gx::ControlEvent> queue;
  gx::LedFrame frame;

 private:
  void push(uint8_t group, uint8_t index, bool pressed, uint8_t velocity = 0) {
    gx::ControlEvent e = {group, index, pressed, 0, false, velocity};
    queue.push_back(e);
  }
};

// A GroovixApp wired to fakes. The storage is passed in so it can outlive the app.
struct Fixture {
  explicit Fixture(MemoryStorage& storage) : app(surface, storage, sink) {}
  FakeSurface surface;
  RecordingSink sink;
  gx::GroovixApp app;
};

bool sameColor(gx::Rgb a, gx::Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

void tapWithShift(FakeSurface& surface, uint8_t button) {
  surface.press(gx::kGroupShift, 0);
  surface.tapButton(button);
  surface.release(gx::kGroupShift, 0);
}

// Squared distance between two colours, for checking how near a surface gets to what the UI
// asked for once its palette or its LEDs have had their say.
uint32_t colorError(const gx::Rgb& got, const gx::Rgb& want) {
  const int dr = static_cast<int>(got.r) - want.r;
  const int dg = static_cast<int>(got.g) - want.g;
  const int db = static_cast<int>(got.b) - want.b;
  return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

// The brightest channel of a colour: the mixer panel's LEDs are on or off, and anything
// below 64 counts as off there.
uint8_t brightness(const gx::Rgb& c) {
  const uint8_t rg = c.r > c.g ? c.r : c.g;
  return rg > c.b ? rg : c.b;
}

bool sameProject(const gx::Project& a, const gx::Project& b) {
  for (int s = 0; s < gx::kNumSongSteps; ++s) {
    if (a.song[s] != b.song[s]) return false;
  }
  for (int s = 0; s < gx::kNumScenes; ++s) {
    if (a.scenes[s].used != b.scenes[s].used || a.scenes[s].muted != b.scenes[s].muted) {
      return false;
    }
    for (int t = 0; t < gx::kNumTracks; ++t) {
      if (a.scenes[s].patterns[t] != b.scenes[s].patterns[t]) return false;
    }
  }
  if (a.bpm != b.bpm || a.swing != b.swing || a.scaleRoot != b.scaleRoot ||
      a.scale != b.scale) {
    return false;
  }
  for (int t = 0; t < gx::kNumTracks; ++t) {
    const gx::Track& ta = a.tracks[t];
    const gx::Track& tb = b.tracks[t];
    if (ta.selectedPattern != tb.selectedPattern || ta.preset != tb.preset ||
        ta.note != tb.note || ta.keyboardOctave != tb.keyboardOctave ||
        ta.keyboardLayout != tb.keyboardLayout || ta.scaleRoot != tb.scaleRoot ||
        ta.scale != tb.scale || ta.pianoRoll != tb.pianoRoll ||
        ta.midiChannel != tb.midiChannel || ta.midiPort != tb.midiPort ||
        ta.instrument != tb.instrument) {
      return false;
    }
    for (int p = 0; p < gx::kNumPatterns; ++p) {
      if (ta.patterns[p].length != tb.patterns[p].length) return false;
      for (int s = 0; s < gx::kMaxSteps; ++s) {
        const gx::Step& sa = ta.patterns[p].steps[s];
        const gx::Step& sb = tb.patterns[p].steps[s];
        if (sa.active != sb.active || sa.noteCount != sb.noteCount ||
            sa.velocity != sb.velocity || sa.probability != sb.probability ||
            sa.gate != sb.gate) {
          return false;
        }
        for (int n = 0; n < gx::kMaxStepNotes; ++n) {
          if (sa.notes[n] != sb.notes[n]) return false;
        }
      }
    }
  }
  return true;
}

// ---- Engine ----

void testSequencerPlaysNotesOnTime() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.toggleStep(0, 0);
  seq.toggleStep(0, 1);
  seq.setStepNote(1, 1, 50);
  seq.play();

  seq.update(1000);  // the first step plays immediately
  CHECK(sink.events.size() == 1);
  CHECK(sink.is(0, kNoteOn, 0, 36));

  seq.update(1124);  // 16ths at 120 BPM are 125 ms apart
  CHECK(sink.events.size() == 1);
  seq.update(1125);
  CHECK(seq.playhead(0) == 1);
  CHECK(sink.events.size() == 4);  // off track 0, on track 0, on track 1
  CHECK(sink.is(1, kNoteOff, 0, 36));
  CHECK(sink.is(3, kNoteOn, 1, 50));

  seq.stop();  // releases every sounding note
  CHECK(sink.events.size() == 6);
  CHECK(sink.is(4, kNoteOff, 0, 36) && sink.is(5, kNoteOff, 1, 50));
}

void testTempoDoesNotDrift() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.play();
  for (uint32_t t = 0; t <= 59875; ++t) seq.update(t);
  CHECK(seq.playhead(0) == 479 % gx::kDefaultPatternLength);  // 59.875 s at 120 BPM

  seq.setBpm(1000);
  CHECK(seq.bpm() == gx::kMaxBpm);
}

void testTracksWrapAtTheirOwnLength() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  static gx::Project project;
  gx::initProject(project);
  project.tracks[2].patterns[0].length = 16;
  seq.setProject(project);
  seq.play();
  for (uint32_t t = 0; t <= 20 * 125; t += 5) seq.update(t);
  CHECK(seq.playhead(0) == 20);
  CHECK(seq.playhead(2) == 4);
}

void testPatterns() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.toggleStep(3, 4);
  CHECK(seq.patternHasData(3, 0) && !seq.patternHasData(3, 1));

  seq.selectPattern(3, 1);
  CHECK(seq.selectedPattern(3) == 1);
  CHECK(!seq.stepActive(3, 4));  // steps belong to the selected pattern

  seq.copyPattern(3, 0, 5, 7);
  CHECK(seq.patternHasData(5, 7));
  seq.clearPattern(3, 0);
  CHECK(!seq.patternHasData(3, 0) && seq.patternHasData(5, 7));

  seq.selectPattern(3, gx::kNumPatterns);  // out of range is ignored
  CHECK(seq.selectedPattern(3) == 1);
}

void testStepEditing() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.setStepNote(0, 2, 60);
  CHECK(seq.stepActive(0, 2) && seq.stepNote(0, 2) == 60);

  seq.toggleStep(0, 3);  // new steps get the last assigned note
  CHECK(seq.stepActive(0, 3) && seq.stepNote(0, 3) == 60);

  seq.copyStep(0, 2, 9);
  CHECK(seq.stepActive(0, 9) && seq.stepNote(0, 9) == 60);
  seq.clearStep(0, 2);
  CHECK(!seq.stepActive(0, 2));
  seq.toggleStep(0, 3);
  CHECK(!seq.stepActive(0, 3));
}

void testRecordQuantisesToNearestStep() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.play();
  seq.update(0);
  seq.recordNote(0, 40);
  CHECK(!seq.patternHasData(0, 0));  // not recording yet

  seq.setRecording(true);
  seq.update(30);  // early in step 1
  seq.recordNote(0, 41);
  CHECK(seq.stepActive(0, 0) && seq.stepNote(0, 0) == 41);
  seq.update(100);  // late in step 1: belongs to step 2
  seq.recordNote(0, 42);
  CHECK(seq.stepActive(0, 1) && seq.stepNote(0, 1) == 42);

  seq.stop();
  seq.recordNote(0, 43);  // stopped: ignored
  CHECK(seq.stepNote(0, 0) == 41);

  // How hard the pad was hit becomes the step's velocity, so a part keeps its accents. A
  // surface that cannot tell sends 0, and the step keeps the velocity it had.
  seq.play();
  seq.setRecording(true);
  seq.update(1000);
  const uint16_t step = seq.playhead(0);
  seq.recordNote(0, 50, 112);
  CHECK(seq.stepVelocity(0, step) == 112);
  seq.recordNote(0, 52, 0);  // the same step, from a mouse
  CHECK(seq.stepHasNote(0, step, 52) && seq.stepVelocity(0, step) == 112);
  seq.recordNote(0, 53, 20);  // a softer hit into the same step speaks for the chord
  CHECK(seq.stepVelocity(0, step) == 20);
  seq.stop();
}

void testPresets() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.setTrackPreset(2, 300);
  CHECK(seq.trackPreset(2) == 300);
  CHECK(sink.is(sink.events.size() - 1, kPreset, 2, 300));

  static gx::Project project;
  gx::initProject(project);
  project.tracks[4].preset = 5;
  sink.events.clear();
  seq.setProject(project);  // announces every track's preset
  // Each track's port, channel and instrument, then its preset.
  CHECK(sink.events.size() == 4u * gx::kNumTracks);
  CHECK(sink.is(16, kPort, 4, 0) && sink.is(17, kChannel, 4, 4));
  CHECK(sink.is(18, kInstrument, 4, gx::kNoInstrument) && sink.is(19, kPreset, 4, 5));
}

// ---- Storage ----

// Builds an older project file as a build with the given capacities would write it: one
// track, holding one pattern, holding one active step. Version 1 by default; a later version
// writes the parts that version added, so the fixture is a real file of that era.
std::vector<uint8_t> sparseProjectFile(uint8_t tracks, uint8_t patterns, uint16_t steps,
                                       uint16_t bpm, uint8_t trackIndex = 1,
                                       uint8_t patternIndex = 2, uint16_t stepIndex = 3,
                                       uint8_t version = 1) {
  std::vector<uint8_t> f;
  const char* magic = "GXPJ";
  f.insert(f.end(), magic, magic + 4);
  f.push_back(version);
  f.push_back(static_cast<uint8_t>(bpm & 0xFF));
  f.push_back(static_cast<uint8_t>(bpm >> 8));
  f.push_back(0);                   // scale root
  f.push_back(gx::kScaleChromatic);  // scale
  f.push_back(tracks);
  f.push_back(patterns);
  f.push_back(static_cast<uint8_t>(steps & 0xFF));
  f.push_back(static_cast<uint8_t>(steps >> 8));
  if (version >= 5) f.push_back(gx::kDefaultSwing);  // the header grew a byte with swing

  std::vector<uint8_t> trackBits((tracks + 7) / 8, 0);
  trackBits[trackIndex >> 3] |= static_cast<uint8_t>(1u << (trackIndex & 7));
  f.insert(f.end(), trackBits.begin(), trackBits.end());

  const uint8_t header[] = {0,  9, 0, 60, 3, 0, 0, gx::kNoOwnScale, 0, 1, 0, gx::kNoInstrument};
  f.insert(f.end(), header, header + sizeof(header));

  std::vector<uint8_t> patternBits((patterns + 7) / 8, 0);
  patternBits[patternIndex >> 3] |= static_cast<uint8_t>(1u << (patternIndex & 7));
  f.insert(f.end(), patternBits.begin(), patternBits.end());

  f.push_back(static_cast<uint8_t>(steps & 0xFF));  // the pattern runs its full length
  f.push_back(static_cast<uint8_t>(steps >> 8));
  std::vector<uint8_t> stepBits((steps + 7) / 8, 0);
  stepBits[stepIndex >> 3] |= static_cast<uint8_t>(1u << (stepIndex & 7));
  f.insert(f.end(), stepBits.begin(), stepBits.end());

  f.push_back(1);   // active
  f.push_back(1);   // one note
  f.push_back(64);
  for (int n = 1; n < gx::kMaxStepNotes; ++n) f.push_back(0);
  f.push_back(gx::kDefaultVelocity);
  f.push_back(gx::kMaxProbability);
  f.push_back(gx::kDefaultGateUnits);
  if (version >= 5) f.push_back(static_cast<uint8_t>(gx::kDefaultNudge));  // micro-timing

  // An empty scene section, and an empty song after it.
  if (version >= 2) {
    f.push_back(gx::kNumScenes);
    for (size_t i = 0; i < (gx::kNumScenes + 7u) / 8u; ++i) f.push_back(0);
  }
  if (version >= 3) {
    f.push_back(gx::kNumSongSteps);
    for (size_t i = 0; i < (gx::kNumSongSteps + 7u) / 8u; ++i) f.push_back(0);
  }
  return f;
}

void testProjectCodec() {
  static gx::Project project;
  gx::initProject(project);
  project.bpm = 133;
  project.tracks[5].selectedPattern = 3;
  project.tracks[5].preset = 300;
  project.tracks[5].patterns[3].length = 12;
  project.tracks[5].patterns[3].steps[3].active = 1;
  project.tracks[5].patterns[3].steps[3].noteCount = 2;  // a chord survives the round trip
  project.tracks[5].patterns[3].steps[3].notes[0] = 61;
  project.tracks[5].patterns[3].steps[3].notes[1] = 64;
  project.tracks[5].patterns[3].steps[3].velocity = 77;
  project.tracks[5].patterns[3].steps[3].probability = 25;
  project.tracks[5].patterns[3].steps[3].gate = 21;
  project.scaleRoot = 9;
  project.scale = gx::kScaleDorian;
  project.tracks[3].keyboardOctave = 5;
  project.tracks[3].keyboardLayout = gx::kKeyboardDrums;
  project.tracks[6].keyboardLayout = gx::kKeyboardOwnScale;
  project.tracks[6].scaleRoot = 4;
  project.tracks[6].scale = gx::kScaleDorian;
  project.tracks[2].pianoRoll = 1;
  project.tracks[5].midiChannel = 9;
  project.tracks[5].midiPort = 3;
  project.tracks[4].instrument = 11;
  project.tracks[7].patterns[0].steps[9].velocity = 100;  // edited but switched off
  project.scenes[0].used = 1;
  project.scenes[0].muted = 0x5;  // tracks 1 and 3 silent
  project.scenes[31].used = 1;
  project.scenes[31].muted = 0;  // a scene where everything plays is still a scene
  project.song[0] = 0;
  project.song[1] = 31;
  project.song[5] = 0;  // the same scene twice makes a longer section

  static uint8_t buffer[gx::kMaxEncodedProjectSize];
  const size_t size = gx::encodeProject(project, buffer, sizeof(buffer));
  static gx::Project decoded;
  gx::initProject(decoded);
  CHECK(gx::decodeProject(buffer, size, decoded));
  CHECK(sameProject(project, decoded));

  // Sparse: only what differs from a new project is written, so a handful of edited steps
  // takes a tiny fraction of what a full one would.
  CHECK(size < 1024 && gx::kMaxEncodedProjectSize > 100 * size);
  CHECK(gx::encodeProject(project, buffer, size - 1) == 0);  // no room

  // An untouched project is just the header and an empty track bitmap.
  static gx::Project empty;
  gx::initProject(empty);
  const size_t emptySize = gx::encodeProject(empty, buffer, sizeof(buffer));
  // The header, an empty track bitmap, and the scene and song counts with their bitmaps.
  CHECK(emptySize == gx::kProjectHeaderSize + gx::kTrackBitmapSize + 1 + gx::kSceneBitmapSize +
                         1 + gx::kSongBitmapSize);
  gx::initProject(decoded);
  decoded.tracks[1].preset = 7;
  CHECK(gx::decodeProject(buffer, emptySize, decoded) && sameProject(empty, decoded));

  // Bad data is refused, and leaves the project it was given alone.
  static gx::Project defaults;
  gx::initProject(defaults);
  static gx::Project untouched;
  untouched = defaults;
  const size_t good = gx::encodeProject(project, buffer, sizeof(buffer));
  CHECK(!gx::decodeProject(buffer, good - 1, untouched));  // truncated
  buffer[0] = 'X';
  CHECK(!gx::decodeProject(buffer, good, untouched));  // bad magic
  buffer[0] = 'G';
  buffer[4] = 9;  // a version this build doesn't know
  CHECK(!gx::decodeProject(buffer, good, untouched));
  buffer[4] = 1;
  buffer[5] = 0;  // bpm 0
  CHECK(!gx::decodeProject(buffer, good, untouched));
  CHECK(sameProject(untouched, defaults));

  // Files written by builds with other capacities.
  std::vector<uint8_t> small = sparseProjectFile(8, 8, 32, 133);
  gx::initProject(decoded);
  decoded.tracks[1].preset = 77;  // must be replaced by the file's value
  CHECK(gx::decodeProject(&small[0], small.size(), decoded));
  CHECK(decoded.bpm == 133 && decoded.tracks[1].preset == 9);
  CHECK(decoded.tracks[1].note == 60 && decoded.tracks[1].keyboardOctave == 3);
  CHECK(decoded.tracks[1].patterns[2].length == 32);
  CHECK(decoded.tracks[1].patterns[2].steps[3].active == 1);
  CHECK(decoded.tracks[1].patterns[2].steps[3].notes[0] == 64);
  // Everything the file leaves out keeps its default.
  CHECK(decoded.tracks[1].patterns[2].steps[4].active == 0);
  CHECK(decoded.tracks[0].patterns[0].length == gx::kDefaultPatternLength);
  CHECK(decoded.tracks[0].midiChannel == gx::defaultMidiChannel(0));
  CHECK(decoded.tracks[1].patterns[3].steps[3].gate == gx::kDefaultGateUnits);

  // A version 5 file - the last before one key, one chord - opens too. Its track headers are
  // a byte shorter, so every track comes up playing single notes, and its steps carry the
  // micro-timing version 5 added.
  std::vector<uint8_t> v5 = sparseProjectFile(8, 8, 32, 133, 1, 2, 3, 5);
  gx::initProject(decoded);
  decoded.tracks[1].chord = gx::kChordSeventh;  // must be replaced by the file's default
  CHECK(gx::decodeProject(&v5[0], v5.size(), decoded));
  CHECK(decoded.tracks[1].chord == gx::kChordOff && decoded.tracks[0].chord == gx::kChordOff);
  CHECK(decoded.tracks[1].preset == 9 && decoded.swing == gx::kDefaultSwing);
  CHECK(decoded.tracks[1].patterns[2].steps[3].nudge == gx::kDefaultNudge);

  // A file with more tracks than this build keeps its first kNumTracks and drops the rest.
  std::vector<uint8_t> wide =
      sparseProjectFile(static_cast<uint8_t>(gx::kNumTracks + 2), 3, 4, 140,
                        static_cast<uint8_t>(gx::kNumTracks + 1));
  gx::initProject(decoded);
  CHECK(gx::decodeProject(&wide[0], wide.size(), decoded));
  CHECK(decoded.bpm == 140 && sameProject(decoded, defaults) == false);

  // One claiming more patterns or steps than this build is refused rather than losing them.
  std::vector<uint8_t> tooManyPatterns =
      sparseProjectFile(8, static_cast<uint8_t>(gx::kNumPatterns + 1), 32, 120);
  gx::initProject(decoded);
  CHECK(!gx::decodeProject(&tooManyPatterns[0], tooManyPatterns.size(), decoded));
  std::vector<uint8_t> tooManySteps =
      sparseProjectFile(8, 8, static_cast<uint16_t>(gx::kMaxSteps + 1), 120);
  CHECK(!gx::decodeProject(&tooManySteps[0], tooManySteps.size(), decoded));

  // Trailing rubbish means the file isn't what it claims to be.
  small.push_back(0);
  CHECK(!gx::decodeProject(&small[0], small.size(), decoded));
}

// Stands in for whatever a platform keeps beside a project — plugin settings, in practice.
class RecordingExtras : public gx::ProjectExtras {
 public:
  void saveProject(uint16_t slot) override { note("save", slot); }
  void openProject(uint16_t slot) override { note("open", slot); }
  void clearProject(uint16_t slot) override { note("clear", slot); }

  std::vector<std::string> calls;

 private:
  void note(const char* what, uint16_t slot) {
    char line[32];
    std::snprintf(line, sizeof(line), "%s %u", what, static_cast<unsigned>(slot));
    calls.push_back(line);
  }
};

void testProjectExtras() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  RecordingExtras extras;
  f.app.setProjectExtras(&extras);
  f.app.begin();
  extras.calls.clear();

  // An empty project frees its slot, and whatever was beside it goes too.
  CHECK(f.app.saveProject());
  CHECK(extras.calls.size() == 1 && extras.calls[0] == "clear 0");

  // With something in it, the extras are saved alongside.
  extras.calls.clear();
  f.surface.tapPad(0);
  f.app.update(0);
  CHECK(f.app.saveProject());
  CHECK(extras.calls.size() == 1 && extras.calls[0] == "save 0");

  // Opening another project saves the old one's extras first, then loads the new one's.
  extras.calls.clear();
  f.app.selectProject(5);
  CHECK(extras.calls.size() == 2);
  CHECK(extras.calls[0] == "save 0" && extras.calls[1] == "open 5");

  // Clearing a slot clears them too, whichever slot it is.
  extras.calls.clear();
  f.app.clearProject(3);
  CHECK(extras.calls.size() == 1 && extras.calls[0] == "clear 3");
}

void testSlotStore() {
  MemoryStorage storage;
  const uint8_t data[] = {7, 8};
  storage.write("thing_05", data, sizeof(data));
  storage.write("thing_150", data, sizeof(data));

  gx::SlotStore slots(storage, "thing", 200);
  slots.scan();
  CHECK(slots.hasData(5) && slots.hasData(150) && !slots.hasData(6));
  CHECK(slots.anyData(128, 64) && !slots.anyData(64, 64));

  uint8_t scratch[4];
  CHECK(slots.copy(5, 12, scratch, sizeof(scratch)));
  CHECK(slots.hasData(12) && storage.blobs.count("thing_12") == 1);
  CHECK(slots.copy(6, 12, scratch, sizeof(scratch)));  // an empty source empties the target
  CHECK(!slots.hasData(12) && storage.blobs.count("thing_12") == 0);
  CHECK(slots.remove(5) && !slots.hasData(5));
  CHECK(!slots.write(200, data, sizeof(data)));  // out of range
}

bool isFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file) std::fclose(file);
  return file != NULL;
}

void testFileStorage(const std::string& dir) {
  gx::FileStorage storage(dir);
  const uint8_t data[] = {1, 2, 3, 4};
  uint8_t buffer[8];
  size_t size = 0;

  CHECK(storage.write("gx_test", data, sizeof(data)));
  CHECK(storage.exists("gx_test"));
  CHECK(storage.read("gx_test", buffer, sizeof(buffer), size) && size == sizeof(data) &&
        std::memcmp(buffer, data, sizeof(data)) == 0);
  CHECK(!storage.read("gx_test", buffer, 3, size));  // does not fit

  const uint8_t replacement[] = {9};
  CHECK(storage.write("gx_test", replacement, sizeof(replacement)));
  CHECK(storage.read("gx_test", buffer, sizeof(buffer), size) && size == 1 && buffer[0] == 9);

  CHECK(storage.remove("gx_test") && !storage.exists("gx_test"));
  CHECK(storage.remove("gx_test"));  // already gone
  CHECK(!storage.read("gx_test", buffer, sizeof(buffer), size));
  CHECK(storage.lastDiscardPath().empty());  // plain removes keep nothing: no trash from them

  // Clearing a slot discards rather than deletes: the file moves into a dated trash folder
  // with its bytes intact, so a mistaken Clear can be undone by hand.
  CHECK(storage.write("gx_test", data, sizeof(data)));
  CHECK(storage.discard("gx_test") && !storage.exists("gx_test"));
  const std::string kept = storage.lastDiscardPath();
  CHECK(kept.find("trash/") != std::string::npos);
  CHECK(kept.find("gx_test.gxb") != std::string::npos);
  std::FILE* file = std::fopen(kept.c_str(), "rb");
  CHECK(file != NULL);
  if (file) {
    const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);
    CHECK(read == sizeof(data) && std::memcmp(buffer, data, sizeof(data)) == 0);
  }

  // A second clear of the same key in the same second gets its own name in the same folder:
  // both are kept, neither is overwritten, and neither falls back to being deleted.
  CHECK(storage.write("gx_test", replacement, sizeof(replacement)));
  CHECK(storage.discard("gx_test"));
  const std::string second = storage.lastDiscardPath();
  CHECK(!second.empty() && second != kept);
  CHECK(isFile(second) && isFile(kept));  // the earlier one is still there

  // Discarding what isn't there succeeds and leaves nothing behind.
  CHECK(storage.discard("gx_test") && storage.lastDiscardPath().empty());

  // The path that matters: emptying a slot is a clear, so it goes through discard. This is
  // what R5 + a pad in Project mode reaches, and the only place the trash comes from.
  gx::SlotStore slots(storage, "gx_slot", 8);
  slots.scan();
  CHECK(slots.write(3, data, sizeof(data)) && slots.hasData(3));
  CHECK(slots.remove(3) && !slots.hasData(3));
  const std::string cleared = storage.lastDiscardPath();
  CHECK(!cleared.empty());  // deleted outright if this is empty
  CHECK(cleared.find("gx_slot_03.gxb") != std::string::npos && isFile(cleared));

  // Copying an empty slot over a full one empties it, which is just as destructive.
  CHECK(slots.write(4, data, sizeof(data)));
  uint8_t scratch[16];
  CHECK(slots.copy(5, 4, scratch, sizeof(scratch)) && !slots.hasData(4));
  CHECK(storage.lastDiscardPath().find("gx_slot_04.gxb") != std::string::npos);
}

// ---- Comm ----

void testMidiEventSink() {
  RecordingMidi midi;
  gx::MidiEventSink sink(midi);
  sink.noteOn(2, 40, 90);
  sink.noteOff(2, 40);
  sink.presetChanged(3, 300);  // bank 2, program 44
  // Both halves of Bank Select, then the Program Change that latches them. The LSB is sent
  // even though it is zero: leaving it out would take whatever the synth was last given.
  CHECK(midi.sent.size() == 5);
  CHECK(midi.sent[0].status == 0x92 && midi.sent[0].data1 == 40 && midi.sent[0].data2 == 90);
  CHECK(midi.sent[1].status == 0x82 && midi.sent[1].data1 == 40);
  CHECK(midi.sent[2].status == 0xB3 && midi.sent[2].data1 == 0 && midi.sent[2].data2 == 2);
  CHECK(midi.sent[3].status == 0xB3 && midi.sent[3].data1 == 32 && midi.sent[3].data2 == 0);
  CHECK(midi.sent[4].status == 0xC3 && midi.sent[4].data1 == 44);

  CHECK(midi.sent[0].port == 0);  // tracks 1-8 go out of port 1

  // Track 2 moves to port 3: its notes and preset go out there.
  sink.trackPortChanged(2, 2);
  sink.noteOn(2, 40, 90);
  sink.presetChanged(2, 5);
  CHECK(midi.sent.size() == 9);
  CHECK(midi.sent[5].port == 2 && midi.sent[6].port == 2);
  CHECK(midi.sent[7].port == 2 && midi.sent[8].port == 2);
  sink.trackPortChanged(2, 0);
  midi.sent.clear();

  // Track 2 moves to channel 10 (9): its notes and preset follow.
  sink.trackChannelChanged(2, 9);
  sink.noteOn(2, 36, 100);
  sink.presetChanged(2, 0);
  CHECK(midi.sent.size() == 4);
  CHECK(midi.sent[0].status == 0x99 && midi.sent[0].data1 == 36);
  CHECK(midi.sent[1].status == 0xB9 && midi.sent[1].data1 == 0);
  CHECK(midi.sent[2].status == 0xB9 && midi.sent[2].data1 == 32);
  CHECK(midi.sent[3].status == 0xC9);

  // A CC goes out on the track's channel and port.
  midi.sent.clear();
  sink.trackPortChanged(2, 3);
  sink.controlChange(2, 74, 100);
  CHECK(midi.sent.size() == 1);
  CHECK(midi.sent[0].status == 0xB9 && midi.sent[0].data1 == 74);
  CHECK(midi.sent[0].data2 == 100 && midi.sent[0].port == 3);

  // A track on an internal instrument sends no MIDI, and reaches the instruments once some
  // are attached.
  RecordingInstruments instruments;
  midi.sent.clear();
  sink.trackInstrumentChanged(2, 5);
  sink.noteOn(2, 60, 100);
  sink.controlChange(2, 74, 10);
  sink.presetChanged(2, 3);
  CHECK(midi.sent.empty());

  sink.setInstruments(&instruments);
  sink.noteOn(2, 61, 90);
  sink.noteOff(2, 61);
  sink.presetChanged(2, 4);
  CHECK(midi.sent.empty() && instruments.events.size() == 3);
  CHECK(instruments.events[0] == "on 5 61 90");
  CHECK(instruments.events[1] == "off 5 61");
  CHECK(instruments.events[2] == "preset 5 4");

  // Back on its MIDI port it plays MIDI again.
  sink.trackInstrumentChanged(2, gx::kNoInstrument);
  sink.noteOn(2, 62, 80);
  CHECK(midi.sent.size() == 1 && instruments.events.size() == 3);
}

// ---- Audio ----

#ifdef GX_HAVE_AUDIO

// How loud a rendered block is, 0 for silence.
double blockLevel(const float* samples, uint32_t frames) {
  double sum = 0;
  for (uint32_t i = 0; i < frames; ++i) sum += samples[i] * samples[i];
  return frames ? std::sqrt(sum / frames) : 0.0;
}

// Levels are measured over one short block, so the reading wobbles a little with where the
// wave happens to start. Compare them with room for that rather than exactly.
bool nearLevel(double a, double b, double tolerance) {
  const double bigger = a > b ? a : b;
  return bigger == 0.0 ? a == b : (a > b ? a - b : b - a) / bigger <= tolerance;
}

// Renders blocks until one of them makes a sound, or gives up. Returns the level.
double renderUntilSound(gx::NullBackend& backend, uint32_t maxBlocks) {
  double level = 0;
  for (uint32_t i = 0; i < maxBlocks && level == 0; ++i) {
    backend.renderBlock();
    level = blockLevel(backend.left(), backend.blockSize());
  }
  return level;
}

void testSpscQueue() {
  gx::SpscQueue<int, 4> queue;
  int value = 0;
  CHECK(queue.empty() && !queue.pop(value));

  CHECK(queue.push(1) && queue.push(2) && queue.push(3));
  CHECK(!queue.push(4));  // one slot is always left open, so a queue of 4 holds 3
  CHECK(!queue.empty());
  CHECK(queue.pop(value) && value == 1);
  CHECK(queue.pop(value) && value == 2);
  CHECK(queue.push(4));  // room again
  CHECK(queue.pop(value) && value == 3);
  CHECK(queue.pop(value) && value == 4);
  CHECK(queue.empty() && !queue.pop(value));

  // Wrapping around the end many times keeps the order.
  for (int round = 0; round < 100; ++round) {
    CHECK(queue.push(round));
    CHECK(queue.pop(value) && value == round);
  }
}

void testAudioEngine() {
  gx::AudioEngine engine;
  gx::NullBackend backend(48000, 128);
  CHECK(backend.start(engine));
  backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) == 0.0);  // silent until something plays

  // A note sounds, and keeps sounding once its short attack ramp is over.
  engine.noteOn(0, 60, 100);
  CHECK(renderUntilSound(backend, 4) > 0.0);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  const double level = blockLevel(backend.left(), backend.blockSize());
  CHECK(level > 0.01);
  CHECK(engine.voicesPlaying() == 1);

  // Releasing it fades to silence rather than cutting off with a click.
  engine.noteOff(0, 60);
  backend.renderBlock();
  const double fading = blockLevel(backend.left(), backend.blockSize());
  CHECK(fading > 0.0 && fading < level);
  for (int i = 0; i < 100; ++i) backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) == 0.0);
  CHECK(engine.voicesPlaying() == 0);

  // A note-off for a note that isn't the one sounding is ignored.
  engine.noteOn(1, 64, 100);
  renderUntilSound(backend, 4);
  engine.noteOff(1, 48);
  backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) > 0.01);
  engine.noteOff(1, 64);

  // Two slots at once are louder than one, and both stereo sides carry the mix.
  for (int i = 0; i < 100; ++i) backend.renderBlock();
  engine.noteOn(2, 60, 127);
  const double one = renderUntilSound(backend, 8);
  engine.noteOn(3, 67, 127);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  const double two = blockLevel(backend.left(), backend.blockSize());
  CHECK(two > one && engine.voicesPlaying() == 2);
  CHECK(nearLevel(blockLevel(backend.right(), backend.blockSize()), two, 0.01));
  engine.noteOff(2, 60);
  engine.noteOff(3, 67);
  for (int i = 0; i < 100; ++i) backend.renderBlock();

  // CC 7 sets the slot's level.
  engine.noteOn(4, 60, 127);
  const double full = renderUntilSound(backend, 8);
  engine.controlChange(4, 7, 20);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) < full);
  engine.noteOff(4, 60);

  // Instruments that don't exist are ignored, not written past the end of the rack.
  engine.noteOn(gx::kNumInstruments, 60, 100);
  engine.noteOn(200, 60, 100);
  for (int i = 0; i < 200; ++i) backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) == 0.0);
  CHECK(engine.droppedEvents() == 0);

  // Far more events than the queue holds are counted rather than blocking the sequencer.
  for (int i = 0; i < 1000; ++i) engine.noteOn(0, 60, 100);
  CHECK(engine.droppedEvents() > 0);
}

void testInstrumentConfig() {
  gx::InstrumentConfig config;
  // Defaults: no plugin, full level, centred.
  CHECK(config.slot(0).pluginUri.empty() && config.slot(0).gain == 1.0f);
  CHECK(config.slot(0).pan == 0.0f && config.assignedSlots() == 0);

  const std::string text =
      "# what each slot hosts\n"
      "\n"
      "I1 = http://sfztools.github.io/sfizz\n"
      "I1.gain = 0.5\n"
      "i1.pan = -1     ; lower case, and a comment\n"
      "I16 = urn:example:plugin\n";
  uint16_t errorLine = 0;
  CHECK(config.load(text, &errorLine));
  CHECK(config.slot(0).pluginUri == "http://sfztools.github.io/sfizz");
  CHECK(config.slot(0).gain == 0.5f && config.slot(0).pan == -1.0f);
  CHECK(config.slot(15).pluginUri == "urn:example:plugin");
  CHECK(config.assignedSlots() == 2);

  // Anything that isn't gain or pan belongs to the plugin, kept in the order written.
  const std::string parameters =
      "I2 = urn:example:synth\n"
      "I2.sfzfile = /tmp/drums.sfz\n"
      "I2.volume = -6\n";
  CHECK(config.load(parameters, &errorLine));
  CHECK(config.slot(1).parameters.size() == 2);
  CHECK(config.slot(1).parameters[0].first == "sfzfile");
  CHECK(config.slot(1).parameters[0].second == "/tmp/drums.sfz");
  CHECK(config.slot(1).parameters[1].first == "volume");  // the plugin decides if it exists

  // Bad lines are refused with their line number, and what came before them is kept.
  const char* cases[] = {
      "I0 = plugin\n",          // slots start at 1
      "I17 = plugin\n",         // past the last slot
      "I1.gain = 2\n",          // out of range
      "I1.pan = -4\n",          // out of range
      "I1.gain = loud\n",       // not a number
      "I1\n",                   // no value
      "plugin = thing\n",       // not a slot name
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    errorLine = 0;
    CHECK(!config.load(cases[i], &errorLine) && errorLine == 1);
  }
  CHECK(config.slot(0).gain == 0.5f);  // the good values survived

  // A missing file is reported without a line number.
  errorLine = 99;
  CHECK(!config.loadFile("/tmp/gx-no-such-instruments.conf", &errorLine) && errorLine == 99);

  // Applying it moves a running engine's levels.
  gx::AudioEngine engine;
  config.applyTo(engine);
  CHECK(engine.gain(0) == 0.5f && engine.pan(0) == -1.0f);
  CHECK(engine.gain(1) == 1.0f && engine.pan(1) == 0.0f);
}

void testInstrumentPanning() {
  gx::AudioEngine engine;
  gx::NullBackend backend(48000, 128);
  CHECK(backend.start(engine));

  // Centred: both sides the same.
  engine.noteOn(0, 60, 127);
  CHECK(renderUntilSound(backend, 4) > 0.0);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  const double centreLeft = blockLevel(backend.left(), backend.blockSize());
  CHECK(centreLeft > 0.01);
  CHECK(nearLevel(blockLevel(backend.right(), backend.blockSize()), centreLeft, 0.01));

  // Hard left: the right side goes quiet, the left gets louder.
  engine.setPan(0, -1.0f);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  const double left = blockLevel(backend.left(), backend.blockSize());
  const double right = blockLevel(backend.right(), backend.blockSize());
  CHECK(right < centreLeft / 100.0 && left > centreLeft);

  // CC 10 pans it, as the mixer's knob row does: 127 is hard right.
  engine.controlChange(0, 10, 127);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  CHECK(blockLevel(backend.left(), backend.blockSize()) < centreLeft / 100.0);
  CHECK(blockLevel(backend.right(), backend.blockSize()) > centreLeft);

  // CC 10 at 64 is the centre again.
  engine.controlChange(0, 10, 64);
  for (int i = 0; i < 20; ++i) backend.renderBlock();
  const double back = blockLevel(backend.left(), backend.blockSize());
  CHECK(nearLevel(back, centreLeft, 0.1));
  engine.noteOff(0, 60);
}

void testWavWriter() {
  const std::string path = "/tmp/gx_test.wav";
  gx::WavWriter wav;
  CHECK(wav.open(path, 44100));
  float left[64];
  float right[64];
  for (int i = 0; i < 64; ++i) {
    left[i] = 0.5f;
    right[i] = -2.0f;  // clipped to -1
  }
  wav.write(left, right, 64);
  wav.write(left, right, 64);
  CHECK(wav.framesWritten() == 128);
  CHECK(wav.close());

  std::FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != NULL);
  uint8_t header[44];
  CHECK(std::fread(header, 1, sizeof(header), file) == sizeof(header));
  CHECK(std::memcmp(header, "RIFF", 4) == 0 && std::memcmp(header + 8, "WAVE", 4) == 0);
  CHECK(header[22] == 2);                              // stereo
  CHECK(header[24] == 0x44 && header[25] == 0xAC);     // 44100 Hz
  CHECK(header[34] == 16);                             // bits per sample
  const uint32_t dataBytes = header[40] | (header[41] << 8) | (header[42] << 16);
  CHECK(dataBytes == 128u * 2u * 2u);
  int16_t first[2] = {0, 0};
  CHECK(std::fread(first, 1, sizeof(first), file) == sizeof(first));
  CHECK(first[0] > 16000 && first[0] < 16500);  // 0.5 of full scale
  CHECK(first[1] == -32767);                    // clipped
  std::fclose(file);
  std::remove(path.c_str());
}

// The whole path: the sequencer routes a track to an instrument, and the engine plays it.
void testInstrumentAudioPath() {
  RecordingMidi midi;
  gx::MidiEventSink sink(midi);
  gx::AudioEngine engine;
  sink.setInstruments(&engine);
  gx::NullBackend backend(48000, 128);
  CHECK(backend.start(engine));

  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.setStepNote(0, 0, 48);
  seq.setTrackInstrument(0, 2);
  seq.play();
  seq.update(0);  // step 1 plays

  // An instrument track sends no channel MIDI; only the clock and transport go out.
  for (size_t i = 0; i < midi.sent.size(); ++i) CHECK(midi.sent[i].status >= 0xF8);
  CHECK(renderUntilSound(backend, 8) > 0.01 && engine.voicesPlaying() == 1);
}

#endif  // GX_HAVE_AUDIO

// MIDI clock: one per engine tick, which is 24 per quarter note, between Start and Stop.
void testMidiClock() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  seq.setBpm(120);

  // Playing but not yet updated: nothing has gone out.
  seq.play();
  CHECK(sink.starts == 0 && sink.clocks == 0);
  seq.stop();
  CHECK(sink.stops == 0);  // it never really started

  seq.play();
  seq.update(0);
  CHECK(sink.starts == 1 && sink.clocks == 1 && sink.clocksAtStart == 0);

  // 120 BPM: a quarter note is 500 ms and carries exactly 24 clocks (update takes ms).
  seq.update(500);
  CHECK(sink.clocks == 25);
  seq.update(1000);
  CHECK(sink.clocks == 49);
  seq.stop();
  CHECK(sink.stops == 1 && sink.starts == 1);

  // The rate follows the tempo: at 60 BPM a quarter note takes a second.
  sink.clocks = 0;
  seq.setBpm(60);
  seq.play();
  seq.update(1000);
  CHECK(sink.clocks == 1);
  seq.update(2000);
  CHECK(sink.clocks == 25);
  seq.stop();

  // Every port gets the clock, and the mask keeps one quiet.
  RecordingMidi midi;
  gx::MidiEventSink midiSink(midi);
  midiSink.clockTick();
  CHECK(midi.sent.size() == gx::kNumMidiPorts);
  for (uint8_t port = 0; port < gx::kNumMidiPorts; ++port) {
    CHECK(midi.sent[port].status == gx::kMidiClock && midi.sent[port].port == port);
  }
  midi.sent.clear();
  midiSink.transportStarted();
  midiSink.transportStopped();
  CHECK(midi.sent.size() == 2u * gx::kNumMidiPorts);
  CHECK(midi.sent[0].status == gx::kMidiStart);
  CHECK(midi.sent[gx::kNumMidiPorts].status == gx::kMidiStop);
  midi.sent.clear();
  midiSink.setClockPorts(0x05);  // ports 1 and 3 only
  midiSink.clockTick();
  CHECK(midi.sent.size() == 2 && midi.sent[0].port == 0 && midi.sent[1].port == 2);
}

// ---- App + UI ----

void testTransportButtons() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  CHECK(!f.app.sequencer().playing());

  f.surface.tapButton(gx::kButtonPlay);
  f.surface.tapButton(gx::kButtonRecord);
  f.app.update(0);
  CHECK(f.app.sequencer().playing() && f.app.sequencer().recording());
  CHECK(sameColor(f.surface.frame.right[gx::kButtonPlay], gx::Rgb{0, 255, 0}));

  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(1);
  CHECK(!f.app.sequencer().playing());
}

void testTrackButtonsReturnToNoteMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();

  tapWithShift(f.surface, gx::kButtonParams);  // Shift + R4: preset mode
  f.surface.tap(gx::kGroupBottom, 4);  // B5
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeNote && f.app.ui().selectedTrack() == 4);

  tapWithShift(f.surface, gx::kButtonParams);
  f.surface.tap(gx::kGroupBottom, 4);  // the already selected track returns too
  f.app.update(1);
  CHECK(f.app.ui().mode() == gx::kModeNote && f.app.ui().selectedTrack() == 4);
}

void testShiftLightsWhileHeld() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();

  f.surface.press(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(sameColor(f.surface.frame.shift, gx::kWhite));

  f.surface.release(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(gx::isLit(f.surface.frame.shift) && !sameColor(f.surface.frame.shift, gx::kWhite));
}

void testNoteMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  CHECK(f.app.ui().mode() == gx::kModeNote);

  f.surface.tapPad(5);  // tap a step: on, with the track's note
  f.app.update(0);
  CHECK(seq.stepActive(0, 5) && seq.stepNote(0, 5) == 36);

  // Hold step 7 and press the note pad for 40 (bottom row, column 5).
  const uint8_t notePad = gx::padIndex(7, 4);
  f.surface.press(gx::kGroupPad, 6);
  f.surface.tapPad(notePad);
  f.surface.release(gx::kGroupPad, 6);
  f.app.update(1);
  CHECK(seq.stepActive(0, 6) && seq.stepNote(0, 6) == 40);  // not toggled off on release
  CHECK(f.sink.contains(kNoteOn, 0, 40) && f.sink.contains(kNoteOff, 0, 40));  // auditioned

  f.surface.press(gx::kGroupPad, 6);  // holding a step lights its note green
  f.app.update(2);
  CHECK(sameColor(f.surface.frame.pads[6], gx::kSelectedColor));
  CHECK(sameColor(f.surface.frame.pads[notePad], gx::kSelectedColor));
  f.surface.release(gx::kGroupPad, 6);  // a lit step doesn't switch off: only Clear removes it
  f.app.update(3);
  CHECK(seq.stepActive(0, 6));

  f.surface.tap(gx::kGroupBottom, 1);  // B2
  f.surface.tapPad(0);
  f.app.update(4);
  CHECK(f.app.ui().selectedTrack() == 1);
  CHECK(seq.stepActive(1, 0) && seq.stepNote(1, 0) == 38);

  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(5);
  CHECK(!seq.stepActive(1, 0));

  // The chromatic keyboard keeps 8 semitones per row, so its roots run diagonally.
  f.surface.press(gx::kGroupPad, 7);
  f.surface.tapPad(gx::padIndex(6, 0));  // one row up from C2 is G#2 (44)
  f.surface.release(gx::kGroupPad, 7);
  f.app.update(6);
  CHECK(seq.stepNote(1, 7) == 44);
}

void testProjectMode() {
  MemoryStorage storage;
  {
    std::unique_ptr<Fixture> fOwner(new Fixture(storage));
    Fixture& f = *fOwner;
    f.app.begin();
    const gx::Sequencer& seq = f.app.sequencer();
    f.surface.tapPad(5);  // project 1 gets a step
    f.surface.tapButton(gx::kButtonProject);
    f.surface.tapPad(3);  // open project 4: project 1 is saved first
    f.app.update(0);
    CHECK(f.app.ui().mode() == gx::kModeProject);
    CHECK(f.app.currentProject() == 3);
    CHECK(f.app.projectHasData(0) && !seq.stepActive(0, 5));
    CHECK(sameColor(f.surface.frame.pads[3], gx::kSelectedColor));
    CHECK(sameColor(f.surface.frame.pads[0], gx::kFilledColor));
    CHECK(sameColor(f.surface.frame.pads[10], gx::kEmptyColor));

    f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
    f.surface.tapPad(0);
    f.surface.tapPad(7);
    f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
    f.app.update(1);
    CHECK(f.app.projectHasData(7));

    f.surface.press(gx::kGroupRight, gx::kButtonClear);
    f.surface.tapPad(7);
    f.surface.release(gx::kGroupRight, gx::kButtonClear);
    f.app.update(2);
    CHECK(!f.app.projectHasData(7));

    f.surface.tapPad(0);  // back to project 1
    f.app.update(3);
    CHECK(f.app.currentProject() == 0 && seq.stepActive(0, 5));

    f.surface.tapPad(3);  // leave project 4 open with a step, as on quit
    f.surface.tapButton(gx::kButtonNote);
    f.surface.tapPad(9);
    f.app.update(4);
    CHECK(f.app.saveProject());
  }

  std::unique_ptr<Fixture> reopenedOwner(new Fixture(storage));
  Fixture& reopened = *reopenedOwner;  // the last used project opens again
  reopened.app.begin();
  CHECK(reopened.app.currentProject() == 3);
  CHECK(reopened.app.sequencer().stepActive(0, 9));
}

void testProjectPages() {
  const uint16_t slot308 = 2 * gx::kSlotsPerPage + 7;  // page 3, pad 8
  MemoryStorage storage;
  {
    std::unique_ptr<Fixture> fOwner(new Fixture(storage));
    Fixture& f = *fOwner;
    f.app.begin();
    f.surface.tapPad(5);  // project 1.01 gets a step

    // From note mode, hold R1 and press B3: project mode on page 3.
    f.surface.press(gx::kGroupRight, gx::kButtonProject);
    f.surface.tap(gx::kGroupBottom, 2);
    f.app.update(0);
    CHECK(f.app.ui().mode() == gx::kModeProject && f.app.ui().selectedTrack() == 0);
    CHECK(sameColor(f.surface.frame.bottom[2], gx::kSelectedColor));  // B LEDs show pages
    CHECK(sameColor(f.surface.frame.pads[0], gx::kEmptyColor));  // project 1.01 isn't here

    f.surface.release(gx::kGroupRight, gx::kButtonProject);
    f.surface.tapPad(7);  // open project 3.08
    f.app.update(1);
    CHECK(f.app.currentProject() == slot308);
    CHECK(f.app.projectHasData(0));  // project 1.01 was saved
    CHECK(sameColor(f.surface.frame.pads[7], gx::kSelectedColor));
    CHECK(sameColor(f.surface.frame.bottom[0], gx::trackColor(0)));  // B LEDs show tracks again

    // Shift + B2 in project mode also picks a page, and stays in project mode.
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 1);
    f.app.update(1);
    CHECK(f.app.ui().mode() == gx::kModeProject && f.app.ui().selectedTrack() == 0);
    CHECK(sameColor(f.surface.frame.bottom[1], gx::kSelectedColor));
    CHECK(std::string(f.app.ui().bottomButtonLabel(1)) == "PG2");
    CHECK(!sameColor(f.surface.frame.pads[7], gx::kSelectedColor));  // 3.08 is on page 3
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(1);
    CHECK(sameColor(f.surface.frame.bottom[0], gx::trackColor(0)));

    // The same in preset mode.
    tapWithShift(f.surface, gx::kButtonParams);
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 3);
    f.app.update(1);
    CHECK(f.app.ui().mode() == gx::kModePreset);
    CHECK(sameColor(f.surface.frame.bottom[3], gx::kSelectedColor));
    f.surface.release(gx::kGroupShift, 0);
    f.surface.tapButton(gx::kButtonProject);
    f.app.update(1);

    // Duplicate across pages: pick 1.01 as the source, paste into 8.64.
    f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
    f.surface.holdAndTapTrack(gx::kButtonProject, 0);
    f.surface.tapPad(0);
    f.surface.holdAndTapTrack(gx::kButtonProject, 7);
    f.surface.tapPad(63);
    f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
    f.surface.press(gx::kGroupRight, gx::kButtonProject);
    f.app.update(2);
    CHECK(f.app.projectHasData(8 * gx::kSlotsPerPage - 1));
    CHECK(sameColor(f.surface.frame.bottom[7], gx::kSelectedColor));
    CHECK(sameColor(f.surface.frame.bottom[0], gx::kFilledColor));
    CHECK(sameColor(f.surface.frame.bottom[1], gx::kEmptyColor));
    f.surface.release(gx::kGroupRight, gx::kButtonProject);

    // Returning to project mode shows the open project's page again.
    f.surface.tapButton(gx::kButtonNote);
    f.surface.tapPad(9);  // give project 3.08 a step so it is saved
    f.surface.tapButton(gx::kButtonProject);
    f.app.update(3);
    CHECK(sameColor(f.surface.frame.pads[7], gx::kSelectedColor));
    CHECK(f.app.saveProject());
  }

  std::unique_ptr<Fixture> reopenedOwner(new Fixture(storage));
  Fixture& reopened = *reopenedOwner;  // the session remembers slots past the first page
  reopened.app.begin();
  CHECK(reopened.app.currentProject() == slot308);
  CHECK(reopened.app.sequencer().stepActive(0, 9));
}

void testPatternMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();

  f.surface.tapPad(4);  // track 1, pattern 1 gets a step (note mode)
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.tapPad(gx::padIndex(2, 1));  // track 2 plays pattern 3
  f.app.update(0);
  CHECK(seq.selectedPattern(1) == 2);
  CHECK(sameColor(f.surface.frame.pads[gx::padIndex(2, 1)], gx::kSelectedColor));
  CHECK(sameColor(f.surface.frame.pads[gx::padIndex(0, 1)], gx::kEmptyColor));
  CHECK(sameColor(f.surface.frame.pads[gx::padIndex(0, 0)], gx::kSelectedColor));

  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(gx::padIndex(0, 0));
  f.surface.tapPad(gx::padIndex(5, 0));
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.app.update(1);
  CHECK(seq.patternHasData(0, 5));
  CHECK(sameColor(f.surface.frame.pads[gx::padIndex(5, 0)], gx::kFilledColor));

  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(gx::padIndex(0, 0));
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(2);
  CHECK(!seq.patternHasData(0, 0) && seq.patternHasData(0, 5));
}

void testPresetMode() {
  MemoryStorage storage;
  // A file left in the data folder is not a preset: nothing here reads or writes one.
  const uint8_t stray[] = {1, 2, 3};
  storage.write("preset_12", stray, sizeof(stray));
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();

  f.surface.tap(gx::kGroupBottom, 2);  // B3
  tapWithShift(f.surface, gx::kButtonParams);  // Shift + R4: preset mode
  f.surface.tapPad(9);
  f.app.update(0);
  CHECK(f.app.sequencer().trackPreset(2) == 9);
  CHECK(f.sink.contains(kPreset, 2, 9));  // Bank Select and Program Change for the track
  CHECK(sameColor(f.surface.frame.pads[9], gx::kSelectedColor));
  // A preset is a number the gear resolves, so every other slot looks the same.
  CHECK(sameColor(f.surface.frame.pads[12], gx::kEmptyColor));
  CHECK(sameColor(f.surface.frame.pads[0], gx::kEmptyColor));

  // Clear and Duplicate have nothing to act on, so they do nothing at all here - in
  // particular they don't fall through to changing the track's preset.
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(20);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(12);
  f.surface.tapPad(20);
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.app.update(1);
  CHECK(f.app.sequencer().trackPreset(2) == 9);
  CHECK(sameColor(f.surface.frame.pads[9], gx::kSelectedColor));
  CHECK(sameColor(f.surface.frame.pads[20], gx::kEmptyColor));
}

void testPresetPages() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();

  // Preset mode is Shift + R4; R4 held there picks the page, and a tap of it goes on to the
  // step parameters, which is what R4 means everywhere else.
  tapWithShift(f.surface, gx::kButtonParams);
  f.surface.press(gx::kGroupRight, gx::kButtonParams);
  f.surface.tap(gx::kGroupBottom, 1);  // page 2
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModePreset);
  CHECK(sameColor(f.surface.frame.bottom[1], gx::kSelectedColor));
  // No page of presets holds anything, so the pages that aren't on screen all look alike.
  CHECK(sameColor(f.surface.frame.bottom[0], gx::kEmptyColor));
  CHECK(sameColor(f.surface.frame.bottom[7], gx::kEmptyColor));
  CHECK(sameColor(f.surface.frame.pads[36], gx::kEmptyColor));

  f.surface.release(gx::kGroupRight, gx::kButtonParams);  // it picked a page, so we stay here
  f.app.update(1);
  CHECK(f.app.ui().mode() == gx::kModePreset);
  f.surface.tapPad(9);  // preset 2.10
  f.app.update(2);
  CHECK(f.app.sequencer().trackPreset(0) == gx::kSlotsPerPage + 9);
  CHECK(f.sink.contains(kPreset, 0, gx::kSlotsPerPage + 9));

  // A tap of R4 that picked no page leaves for the step parameters.
  f.surface.tapButton(gx::kButtonParams);
  f.app.update(3);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);

  // Re-entering preset mode opens the page of the track's preset.
  f.surface.tapButton(gx::kButtonNote);
  tapWithShift(f.surface, gx::kButtonParams);
  f.app.update(4);
  CHECK(sameColor(f.surface.frame.pads[9], gx::kSelectedColor));
}

void testAppKeepsDefaultsWhenStorageIsCorrupt() {
  MemoryStorage storage;
  storage.blobs["project_00"].assign(10, 0xFF);
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  CHECK(f.app.currentProject() == 0);
  CHECK(f.app.sequencer().bpm() == gx::kDefaultBpm);
  CHECK(!f.app.sequencer().patternHasData(0, 0));
}

void testOldSessionFormatStillOpens() {
  MemoryStorage storage;
  const uint8_t session[] = {1, 5};  // version 1: {1, slot}
  storage.write("session", session, sizeof(session));
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  CHECK(f.app.currentProject() == 5);
}

// LED of a page the test doesn't use: grey if the build has that page, off if not.
gx::Rgb unusedPageColor(uint8_t numPages, uint8_t page) {
  return page < numPages ? gx::kEmptyColor : gx::kBlack;
}

// B5: the first of the bottom buttons that pattern mode gives to the pattern pages.
const uint8_t kFirstPatternPage = gx::kNumBottomButtons / 2;

void testPatternPages() {
  CHECK(gx::kNumTrackPages >= 2 && gx::kNumPatternPages >= 2);  // uses two pages of each
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;

  f.surface.tapPad(4);  // track 1, pattern 1 gets a step (note mode)
  f.surface.tapButton(gx::kButtonPattern);
  // The bottom row pages the grid: B1..B4 the tracks it shows, B5..B8 the patterns. No Shift,
  // so Shift + R keeps its modes here, and R8 still plays.
  f.surface.tap(gx::kGroupBottom, 1);                             // B2: tracks 9-16
  f.surface.tap(gx::kGroupBottom, kFirstPatternPage + 1);         // B6: patterns 9-16
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModePattern);
  CHECK(sameColor(frame.bottom[1], gx::kSelectedColor));  // B1..B4: track pages
  CHECK(sameColor(frame.bottom[0], gx::kFilledColor));    // tracks 1-8 have data
  CHECK(sameColor(frame.bottom[2], unusedPageColor(gx::kNumTrackPages, 2)));
  CHECK(sameColor(frame.bottom[kFirstPatternPage + 1], gx::kSelectedColor));  // B5..B8: patterns
  CHECK(sameColor(frame.bottom[kFirstPatternPage], gx::kEmptyColor));  // nothing on this page
  CHECK(sameColor(frame.bottom[kFirstPatternPage + 2],
                  unusedPageColor(gx::kNumPatternPages, 2)));
  CHECK(sameColor(frame.right[gx::kButtonPattern], gx::kWhite));  // R LEDs keep their functions

  f.surface.tapPad(gx::padIndex(0, 3));  // track 12 plays pattern 9
  f.app.update(1);
  CHECK(seq.selectedPattern(11) == 8);
  CHECK(sameColor(frame.pads[gx::padIndex(0, 3)], gx::kSelectedColor));
  CHECK(f.app.ui().selectedTrack() == 11);  // the column you touch is the track you selected

  // Duplicate across pages: track 1 pattern 1 -> track 12 pattern 10.
  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tap(gx::kGroupBottom, 0);                      // tracks 1-8
  f.surface.tap(gx::kGroupBottom, kFirstPatternPage);      // patterns 1-8
  f.surface.tapPad(gx::padIndex(0, 0));
  f.surface.tap(gx::kGroupBottom, 1);                      // tracks 9-16
  f.surface.tap(gx::kGroupBottom, kFirstPatternPage + 1);  // patterns 9-16
  f.surface.tapPad(gx::padIndex(1, 3));
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.app.update(2);
  CHECK(seq.patternHasData(11, 9));

  // Shift here means the mute row and nothing else: the bottom buttons go on paging while it
  // is held, so a page is still one press away with a hand on the mutes.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 0);                  // tracks 1-8
  f.surface.tap(gx::kGroupBottom, kFirstPatternPage);  // patterns 1-8
  f.app.update(3);
  CHECK(sameColor(frame.bottom[0], gx::kSelectedColor));
  CHECK(sameColor(frame.bottom[kFirstPatternPage], gx::kSelectedColor));
  f.surface.tapPad(gx::padIndex(7, 2));  // and the pad row still mutes, here track 3
  f.app.update(4);
  CHECK(seq.trackMuted(2));
  f.surface.tap(gx::kGroupBottom, 1);                      // back to tracks 9-16
  f.surface.tap(gx::kGroupBottom, kFirstPatternPage + 1);  // and patterns 9-16
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(5);
  f.surface.tapPad(gx::padIndex(1, 3));  // the pages moved with it: track 12, pattern 10
  f.app.update(6);
  CHECK(seq.selectedPattern(11) == 9);

  // R3 opens note mode on the track whose column was last touched.
  f.surface.tapButton(gx::kButtonNote);
  f.app.update(7);
  CHECK(f.app.ui().mode() == gx::kModeNote && f.app.ui().selectedTrack() == 11);
}

void testTrackPagesInNoteMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  const gx::Sequencer& seq = f.app.sequencer();

  f.surface.tap(gx::kGroupBottom, 3);  // track 4
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 1);  // Shift + B2: tracks 9-16, same position: track 12
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeNote && f.app.ui().selectedTrack() == 11);
  CHECK(sameColor(frame.bottom[1], gx::kSelectedColor));       // B LEDs show track pages
  CHECK(sameColor(frame.right[gx::kButtonNote], gx::kWhite));  // R LEDs keep their functions

  f.surface.release(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(sameColor(frame.bottom[3], gx::trackColor(11)));  // B4 lit as the selected track

  f.surface.tap(gx::kGroupBottom, 7);  // B8 is now track 16
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 0);  // back to tracks 1-8: track 16 -> track 8
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(2);
  CHECK(f.app.ui().selectedTrack() == 7);

  // A step held while flipping pages is dropped, not toggled on either track.
  f.surface.press(gx::kGroupPad, 5);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 1);
  f.surface.release(gx::kGroupShift, 0);
  f.surface.release(gx::kGroupPad, 5);
  f.app.update(3);
  CHECK(f.app.ui().selectedTrack() == 15);
  CHECK(!seq.stepActive(7, 5) && !seq.stepActive(15, 5));
}

void testTrackPageLimit() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t lastPage = gx::kNumTrackPages - 1;

  // Shift + the last page's button keeps the track's position: track 3 of that page.
  f.surface.tap(gx::kGroupBottom, 2);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, lastPage);
  f.app.update(0);
  CHECK(f.app.ui().selectedTrack() == lastPage * gx::kTracksPerPage + 2);
  CHECK(sameColor(frame.bottom[lastPage], gx::kSelectedColor));
  if (gx::kNumTrackPages < gx::kNumBottomButtons) {
    // Buttons past the last track page are dark and do nothing.
    CHECK(sameColor(frame.bottom[gx::kNumTrackPages], gx::kBlack));
    f.surface.tap(gx::kGroupBottom, gx::kNumTrackPages);
    f.app.update(1);
    CHECK(f.app.ui().selectedTrack() == lastPage * gx::kTracksPerPage + 2);
  }
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(2);
  CHECK(f.app.ui().mode() == gx::kModeNote);
}

void testStepPagesAndLength() {
  CHECK(gx::kNumStepPages >= 2);  // the desktop build has 8 pages of 32 steps
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const gx::Rgb kRed = {255, 0, 0};

  CHECK(seq.trackLength(0) == gx::kDefaultPatternLength);  // new patterns are one page
  f.surface.tapPad(4);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapPad(11);  // Shift + step 12 ends the pattern there
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(seq.trackLength(0) == 12);
  CHECK(sameColor(frame.pads[11], kRed));           // the last step is red
  CHECK(sameColor(frame.pads[12], gx::kBlack));     // past the end

  // Hold R3 and press B2 for steps 33-64, then extend the pattern to step 40.
  f.surface.press(gx::kGroupRight, gx::kButtonNote);
  f.surface.tap(gx::kGroupBottom, 1);
  f.app.update(1);
  CHECK(sameColor(frame.bottom[1], gx::kSelectedColor));  // page 2 is shown
  CHECK(sameColor(frame.bottom[0], gx::kFilledColor));    // page 1 holds steps
  f.surface.release(gx::kGroupRight, gx::kButtonNote);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapPad(7);  // step 40
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(3);  // step 36 on
  f.app.update(2);
  CHECK(seq.trackLength(0) == 40);
  CHECK(seq.stepActive(0, 35) && !seq.stepActive(0, 3));
  CHECK(sameColor(frame.pads[7], kRed));
  CHECK(sameColor(frame.pads[0], gx::kEmptyColor) == false);  // step 33 is a normal step

  // While playing, the page holding the playhead blinks on the track buttons.
  f.surface.tapButton(gx::kButtonPlay);
  f.surface.press(gx::kGroupRight, gx::kButtonNote);
  f.app.update(3);
  CHECK(sameColor(frame.bottom[0], gx::kWhite));  // playhead on page 1, blink on
  f.app.update(3 + 250);
  CHECK(sameColor(frame.bottom[0], gx::kFilledColor));  // blink off
  f.surface.release(gx::kGroupRight, gx::kButtonNote);
  f.app.update(600);
  CHECK(f.app.ui().mode() == gx::kModeNote);
}

void testChordPlayback() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  const uint8_t chord[] = {36, 40, 43};
  seq.setStepChord(0, 0, chord, 3);
  CHECK(seq.stepNoteCount(0, 0) == 3 && seq.stepActive(0, 0));

  seq.play();
  seq.update(0);  // the whole chord sounds
  CHECK(sink.events.size() == 3);
  CHECK(sink.is(0, kNoteOn, 0, 36) && sink.is(1, kNoteOn, 0, 40) && sink.is(2, kNoteOn, 0, 43));
  seq.stop();  // and every note of it is released
  CHECK(sink.events.size() == 6);
  CHECK(sink.is(3, kNoteOff, 0, 36) && sink.is(4, kNoteOff, 0, 40) && sink.is(5, kNoteOff, 0, 43));

  seq.addStepNote(0, 0, 40);  // already in the chord
  CHECK(seq.stepNoteCount(0, 0) == 3);
  for (uint8_t n = 0; n < 8; ++n) seq.addStepNote(0, 0, static_cast<uint8_t>(50 + n));
  CHECK(seq.stepNoteCount(0, 0) == gx::kMaxStepNotes);  // the chord is capped

  // Switching a step off keeps its chord, so switching it back on restores it.
  seq.toggleStep(0, 0);
  CHECK(!seq.stepActive(0, 0) && seq.stepNoteCount(0, 0) == gx::kMaxStepNotes);
  seq.toggleStep(0, 0);
  CHECK(seq.stepActive(0, 0) && seq.stepNoteCount(0, 0) == gx::kMaxStepNotes);

  // Notes recorded into one step build a chord instead of overwriting.
  seq.setRecording(true);
  seq.play();
  seq.update(1000);
  seq.recordNote(1, 60);
  seq.recordNote(1, 64);
  CHECK(seq.stepNoteCount(1, 0) == 2);
  CHECK(seq.stepNote(1, 0, 0) == 60 && seq.stepNote(1, 0, 1) == 64);
}

// Plays a sequencer for `loops` steps (125 ms each at 120 BPM) and counts the note-ons.
uint32_t countNoteOns(gx::Sequencer& seq, RecordingSink& sink, uint32_t loops) {
  sink.events.clear();
  seq.play();
  for (uint32_t i = 0; i < loops; ++i) seq.update(i * 125);
  seq.stop();
  uint32_t count = 0;
  for (size_t i = 0; i < sink.events.size(); ++i) {
    if (sink.events[i].type == kNoteOn) ++count;
  }
  return count;
}

void testStepProbability() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;
  const uint32_t kLoops = 400;
  CHECK(seq.stepProbability(0, 0) == gx::kMaxProbability);  // steps always play by default
  seq.setStepNote(0, 0, 36);
  seq.setTrackLength(0, 1);  // the one step plays every time round
  CHECK(countNoteOns(seq, sink, kLoops) == kLoops);

  seq.setStepProbability(0, 0, 50);
  const uint32_t half = countNoteOns(seq, sink, kLoops);
  CHECK(half > kLoops * 35 / 100 && half < kLoops * 65 / 100);

  seq.setStepProbability(0, 0, 0);
  CHECK(seq.stepProbability(0, 0) == 1);
  seq.setStepProbability(0, 0, 200);
  CHECK(seq.stepProbability(0, 0) == gx::kMaxProbability);

  // The same seed rolls the same dice.
  seq.setStepProbability(0, 0, 30);
  seq.seedRandom(1234);
  const uint32_t first = countNoteOns(seq, sink, kLoops);
  seq.seedRandom(1234);
  CHECK(countNoteOns(seq, sink, kLoops) == first);

  // A chord plays whole or not at all.
  const uint8_t chord[] = {36, 40, 43};
  seq.setStepChord(0, 0, chord, 3);
  seq.setStepProbability(0, 0, 50);
  const uint32_t chordNotes = countNoteOns(seq, sink, kLoops);
  CHECK(chordNotes % 3 == 0 && chordNotes > 0 && chordNotes < 3 * kLoops);

  seq.clearStep(0, 0);  // clearing a step resets its probability
  CHECK(seq.stepProbability(0, 0) == gx::kMaxProbability);
}

void testChordEditing() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t keyC = gx::padIndex(7, 0);  // C2, 36
  const uint8_t keyE = gx::padIndex(7, 4);  // 40
  const uint8_t keyG = gx::padIndex(7, 7);  // 43

  // Hold three keys, then press a step: the step takes that chord.
  f.surface.press(gx::kGroupPad, keyC);
  f.surface.press(gx::kGroupPad, keyE);
  f.surface.press(gx::kGroupPad, keyG);
  f.surface.press(gx::kGroupPad, 2);
  f.app.update(0);
  CHECK(seq.stepActive(0, 2) && seq.stepNoteCount(0, 2) == 3);
  CHECK(seq.stepNote(0, 2, 0) == 36 && seq.stepNote(0, 2, 1) == 40 && seq.stepNote(0, 2, 2) == 43);

  f.surface.release(gx::kGroupPad, keyC);
  f.surface.release(gx::kGroupPad, keyE);
  f.surface.release(gx::kGroupPad, keyG);
  f.surface.release(gx::kGroupPad, 2);  // the step took notes, so this must not switch it off
  f.app.update(1);
  CHECK(seq.stepActive(0, 2));

  // Holding the step lights every note of its chord.
  f.surface.press(gx::kGroupPad, 2);
  f.app.update(2);
  CHECK(sameColor(frame.pads[keyC], gx::kSelectedColor));
  CHECK(sameColor(frame.pads[keyE], gx::kSelectedColor));
  CHECK(sameColor(frame.pads[keyG], gx::kSelectedColor));
  f.surface.release(gx::kGroupPad, 2);  // looking at a step leaves it on
  f.app.update(3);
  CHECK(seq.stepActive(0, 2) && seq.stepNoteCount(0, 2) == 3);
  f.surface.tapPad(2);
  f.app.update(4);
  CHECK(seq.stepActive(0, 2) && seq.stepNoteCount(0, 2) == 3);  // tapping it doesn't either

  // Holding a step and tapping keys adds their notes to it.
  f.surface.press(gx::kGroupPad, 5);
  f.surface.tapPad(keyE);
  f.surface.tapPad(keyG);
  f.surface.release(gx::kGroupPad, 5);
  f.app.update(5);
  CHECK(seq.stepNoteCount(0, 5) == 2);
  CHECK(seq.stepNote(0, 5, 0) == 40 && seq.stepNote(0, 5, 1) == 43);

  // Tapping a key the held step already has takes that note out, as on Circuit Tracks; taking
  // out the last one empties the step.
  f.surface.press(gx::kGroupPad, 5);
  f.surface.tapPad(keyE);
  f.surface.release(gx::kGroupPad, 5);
  f.app.update(6);
  CHECK(seq.stepActive(0, 5) && seq.stepNoteCount(0, 5) == 1 && seq.stepNote(0, 5, 0) == 43);
  f.surface.press(gx::kGroupPad, 5);
  f.surface.tapPad(keyG);
  f.surface.release(gx::kGroupPad, 5);
  f.app.update(7);
  CHECK(!seq.stepActive(0, 5) && seq.stepNoteCount(0, 5) == 0);

  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopenedOwner(new Fixture(storage));
  Fixture& reopened = *reopenedOwner;
  reopened.app.begin();  // chords are saved with the project
  CHECK(reopened.app.sequencer().stepNoteCount(0, 2) == 3);
  CHECK(reopened.app.sequencer().stepNote(0, 2, 1) == 40);
}

void testStepParamsMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kVelocityRow = 4;  // rows 5-6: velocity, 16 levels
  const uint8_t kGateRow = 6;      // rows 7-8: gate, a ramp of lengths a pad each

  f.surface.tapPad(0);  // steps 1 and 2 on, in note mode
  f.surface.tapPad(1);
  f.surface.tapButton(gx::kButtonParams);  // R4: the step parameters, not a modifier
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);
  CHECK(sameColor(frame.right[gx::kButtonParams], gx::kWhite));

  // Tap a step to select it, then a velocity pad: the sixth pad of row 5 is 48.
  f.surface.tapPad(0);
  f.surface.tapPad(gx::padIndex(kVelocityRow, 5));
  f.app.update(1);
  CHECK(seq.stepVelocity(0, 0) == 48);
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));
  CHECK(frame.pads[gx::padIndex(kVelocityRow, 5)].r == 255);  // the level itself
  CHECK(frame.pads[gx::padIndex(kVelocityRow, 4)].r == 90);   // the bar below it
  CHECK(frame.pads[gx::padIndex(kVelocityRow, 6)].r == 12);   // the lane above it
  f.surface.tapPad(gx::padIndex(kVelocityRow + 1, 7));  // the lane carries on along row 6: 127
  f.app.update(2);
  CHECK(seq.stepVelocity(0, 0) == 127);
  f.surface.tapPad(gx::padIndex(kVelocityRow, 5));
  f.app.update(2);
  CHECK(seq.stepVelocity(0, 0) == 48);

  // Gate: the lane is one ramp of lengths, a pad each, and a tap is that length. Pads 1-5 are
  // sixths of a step, pad 6 is one step - which is what a new step has - and the rest are whole
  // steps. Nothing is hidden behind a second tap.
  CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);
  CHECK(frame.pads[gx::padIndex(kGateRow, 5)].b == 255);  // one step: the sixth pad
  CHECK(frame.pads[gx::padIndex(kGateRow, 4)].b == 90);   // the bar below it
  CHECK(frame.pads[gx::padIndex(kGateRow, 6)].b == 12);   // the lane above it

  // The short end: one tap reaches any sixth of a step, in either direction.
  f.surface.tapPad(gx::padIndex(kGateRow, 0));
  f.app.update(3);
  CHECK(seq.stepGate(0, 0) == 1);  // a sixth of a step, the shortest there is
  CHECK(frame.pads[gx::padIndex(kGateRow, 0)].b == 255 && frame.pads[gx::padIndex(kGateRow, 1)].b == 12);
  f.surface.tapPad(gx::padIndex(kGateRow, 2));
  f.app.update(4);
  CHECK(seq.stepGate(0, 0) == 3);  // half a step

  // A second tap on the same pad is the same length again, not a shorter one.
  f.surface.tapPad(gx::padIndex(kGateRow, 2));
  f.app.update(5);
  CHECK(seq.stepGate(0, 0) == 3);

  // The long end: whole steps, up to sixteen on the last pad.
  f.surface.tapPad(gx::padIndex(kGateRow, 7));  // the eighth pad: three steps
  f.app.update(6);
  CHECK(seq.stepGate(0, 0) == 3 * gx::kGateUnitsPerStep);
  f.surface.tapPad(gx::padIndex(kGateRow + 1, 7));  // the last pad: 16 steps
  f.app.update(7);
  CHECK(seq.stepGate(0, 0) == gx::kMaxGateUnits);
  CHECK(frame.pads[gx::padIndex(kGateRow + 1, 7)].b == 255 &&
        frame.pads[gx::padIndex(kGateRow + 1, 6)].b == 90);
  f.surface.tapPad(gx::padIndex(kGateRow, 5));  // back to one step
  f.app.update(8);
  CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);

  // Hold two steps to edit both; the earlier selection is replaced.
  f.surface.press(gx::kGroupPad, 1);
  f.surface.press(gx::kGroupPad, 2);
  f.surface.tapPad(gx::padIndex(kVelocityRow + 1, 7));
  f.surface.tapPad(gx::padIndex(kGateRow, 6));  // two steps
  f.surface.release(gx::kGroupPad, 1);
  f.surface.release(gx::kGroupPad, 2);
  f.app.update(8);
  CHECK(seq.stepVelocity(0, 1) == 127 && seq.stepVelocity(0, 2) == 127);
  CHECK(seq.stepGate(0, 1) == 12 && seq.stepGate(0, 2) == 12);
  CHECK(seq.stepVelocity(0, 0) == 48 && seq.stepGate(0, 0) == gx::kDefaultGateUnits);

  // Clear + step resets velocity and gate; Duplicate + step + step copies them.
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(1);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(2);
  f.surface.tapPad(5);
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(7);  // move the selection away so steps 1 and 2 show their brightness
  f.app.update(9);
  CHECK(seq.stepVelocity(0, 1) == gx::kDefaultVelocity && seq.stepGate(0, 1) == gx::kDefaultGateUnits);
  CHECK(seq.stepVelocity(0, 5) == 127 && seq.stepGate(0, 5) == 12);
  CHECK(frame.pads[1].r > frame.pads[0].r);  // velocity 100 is brighter than 48

  // Playback sends the step's velocity.
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(10);
  bool sentVelocity = false;
  for (size_t i = 0; i < f.sink.events.size(); ++i) {
    const SinkEvent& e = f.sink.events[i];
    if (e.type == kNoteOn && e.track == 0 && e.value == 36 && e.velocity == 48) sentVelocity = true;
  }
  CHECK(sentVelocity);
  f.surface.tapButton(gx::kButtonPlay);

  // Hold R4 + B2 for another step page; R4 on its own stays here, since this is its mode.
  f.surface.press(gx::kGroupRight, gx::kButtonParams);
  f.surface.tap(gx::kGroupBottom, 1);
  f.surface.release(gx::kGroupRight, gx::kButtonParams);
  f.app.update(11);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);
  CHECK(sameColor(frame.pads[0], gx::kBlack));  // steps 33+ are past the 32-step pattern
  f.surface.tapButton(gx::kButtonParams);
  f.app.update(12);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);
  tapWithShift(f.surface, gx::kButtonParams);  // Shift + R4 is the presets
  f.app.update(13);
  CHECK(f.app.ui().mode() == gx::kModePreset);

  // R4 opens the step parameters from any mode, pattern mode included: its bottom row pages
  // the grid, so nothing there is in the way.
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.tapButton(gx::kButtonParams);
  f.app.update(14);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);
}

void testStepGate() {
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);
    seq.setStepNote(0, 0, 36);
    seq.setStepGate(0, 0, 3);  // half a step
    seq.setStepNote(0, 2, 38);
    seq.setStepGate(0, 2, 12);  // two steps: over step 4
    seq.setStepNote(0, 3, 40);
    seq.play();
    seq.update(0);
    CHECK(sink.events.size() == 1 && sink.is(0, kNoteOn, 0, 36));
    seq.update(62);  // three ticks are 62.5 ms at 120 BPM
    CHECK(sink.events.size() == 1);
    seq.update(63);
    CHECK(sink.events.size() == 2 && sink.is(1, kNoteOff, 0, 36));
    seq.update(250);  // step 3
    CHECK(sink.events.size() == 3 && sink.is(2, kNoteOn, 0, 38));
    seq.update(375);  // step 4 starts while step 3's note still sounds
    CHECK(sink.events.size() == 4 && sink.is(3, kNoteOn, 0, 40));
    seq.update(500);  // both end together: 38 after two steps, 40 after one
    CHECK(sink.events.size() == 6);
    CHECK(sink.contains(kNoteOff, 0, 38) && sink.contains(kNoteOff, 0, 40));

    seq.setStepGate(0, 0, 200);
    CHECK(seq.stepGate(0, 0) == gx::kMaxGateUnits);
    seq.setStepGate(0, 0, 0);
    CHECK(seq.stepGate(0, 0) == 1);
    seq.clearStep(0, 0);
    CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);
  }
  {
    // A pitch still sounding from a long gate stops before it plays again.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    seq.setStepNote(0, 0, 36);
    seq.setStepGate(0, 0, 12);
    seq.setStepNote(0, 1, 36);
    seq.play();
    seq.update(0);
    seq.update(125);
    CHECK(sink.events.size() == 3);
    CHECK(sink.is(1, kNoteOff, 0, 36) && sink.is(2, kNoteOn, 0, 36));
    seq.stop();  // and stopping releases what's left
    CHECK(sink.events.size() == 4 && sink.is(3, kNoteOff, 0, 36));
  }
}

void testProbabilityMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kProbabilityRow = 4;  // rows 5-6

  f.surface.tapPad(0);  // step 1 on, in note mode
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonClear);  // Shift + R5
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeProbability);
  CHECK(frame.right[gx::kButtonClear].r == 255 && frame.right[gx::kButtonClear].g == 90);

  // Select the step (Shift + R5 didn't leave Clear held), then 50%: pad 8 of row 5.
  f.surface.tapPad(0);
  f.surface.tapPad(gx::padIndex(kProbabilityRow, 7));
  f.app.update(1);
  CHECK(seq.stepActive(0, 0) && seq.stepProbability(0, 0) == 50);
  CHECK(frame.pads[gx::padIndex(kProbabilityRow, 7)].r == 170);
  f.surface.tapPad(gx::padIndex(kProbabilityRow, 0));
  f.app.update(2);
  CHECK(seq.stepProbability(0, 0) == 6);
  f.surface.tapPad(gx::padIndex(kProbabilityRow + 1, 7));
  f.app.update(3);
  CHECK(seq.stepProbability(0, 0) == gx::kMaxProbability);
  // Rows 7-8 are micro-timing. The step is straight, so the pad that means "on the grid"
  // (the first of row 8) is the value, and the rest of the lane is dim.
  CHECK(frame.pads[gx::padIndex(7, 0)].r == 255 && frame.pads[gx::padIndex(7, 0)].g == 60);
  CHECK(gx::isLit(frame.pads[gx::padIndex(6, 0)]) == false ||
        frame.pads[gx::padIndex(6, 0)].r < 255);

  // Hold R5 + B2 for the second page of steps rather than another track; R5 + B1 comes back.
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tap(gx::kGroupBottom, 1);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(4);
  CHECK(f.app.ui().mode() == gx::kModeProbability && f.app.ui().selectedTrack() == 0);
  CHECK(sameColor(frame.pads[0], gx::kBlack));  // steps 33+ are past the 32-step pattern
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tap(gx::kGroupBottom, 0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);

  // R5 + step resets its probability.
  f.surface.tapPad(0);
  f.surface.tapPad(gx::padIndex(kProbabilityRow, 3));  // 25%
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(5);
  CHECK(seq.stepActive(0, 0) && seq.stepProbability(0, 0) == gx::kMaxProbability);

  // Shift + R5 opens probability from pattern mode too.
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonClear);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(6);
  CHECK(f.app.ui().mode() == gx::kModeProbability);
}

void testFaders() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::UiController& ui = f.app.ui();
  f.app.update(0);
  CHECK(ui.faderPosition(0) == 0 && ui.masterFaderPosition() == 0);
  const size_t eventsBefore = f.sink.events.size();

  f.surface.moveFader(gx::kGroupFader, 2, 700);
  f.surface.moveFader(gx::kGroupFader, 7, 5000);  // clamped to the top
  f.surface.moveFader(gx::kGroupFader, gx::kNumTrackFaders, 300);  // no such fader: ignored
  f.surface.moveFader(gx::kGroupMasterFader, 0, 512);
  f.app.update(1);
  CHECK(ui.faderPosition(2) == 700);
  CHECK(ui.faderPosition(7) == gx::kFaderMax);
  CHECK(ui.faderPosition(0) == 0 && ui.faderPosition(gx::kNumTrackFaders) == 0);
  CHECK(ui.masterFaderPosition() == 512);

  // Faders send their CC (see testControlCc) but change nothing else in the UI.
  CHECK(ui.mode() == gx::kModeNote && ui.selectedTrack() == 0);
  for (size_t i = eventsBefore; i < f.sink.events.size(); ++i) {
    CHECK(f.sink.events[i].type == kControl);
  }
}

void testControlMap() {
  gx::ControlMap map;
  // Defaults: the mixer's faders send Volume, its knob rows Cutoff, Resonance and centred Pan,
  // the faders under the pads send Expression, and the masters send nothing.
  CHECK(map.assignment(gx::kGroupMixFader, 0).cc == 7);
  CHECK(map.assignment(gx::kGroupFader, 0).cc == 11);
  CHECK(map.assignment(gx::kGroupKnob, gx::knobIndex(0, 0)).cc == 74);
  CHECK(map.assignment(gx::kGroupKnob, gx::knobIndex(1, 3)).cc == 71);
  const gx::CcAssignment& pan = map.assignment(gx::kGroupKnob, gx::knobIndex(2, 7));
  CHECK(pan.cc == 10 && pan.sweep == gx::kSweepCentre);
  CHECK(map.assignment(gx::kGroupMasterFader, 0).cc == gx::kNoCc);
  CHECK(map.assignment(gx::kGroupMixMaster, 0).cc == gx::kNoCc);

  // Values: full sweep spans 0..127, centred holds 64 around the middle.
  const gx::CcAssignment full = {7, gx::kSweepFull};
  CHECK(gx::ControlMap::ccValue(full, 0) == 0);
  CHECK(gx::ControlMap::ccValue(full, gx::kFaderMax) == 127);
  CHECK(gx::ControlMap::ccValue(full, gx::kFaderMax + 500) == 127);  // clamped
  const gx::CcAssignment centred = {10, gx::kSweepCentre};
  CHECK(gx::ControlMap::ccValue(centred, gx::kFaderMax / 2) == 64);
  CHECK(gx::ControlMap::ccValue(centred, gx::kFaderMax / 2 + 20) == 64);  // the detent
  CHECK(gx::ControlMap::ccValue(centred, 0) == 0);
  CHECK(gx::ControlMap::ccValue(centred, gx::kFaderMax) == 127);

  // A config file.
  const char* config =
      "# comment\n"
      "\n"
      "fader1 = 1\n"
      "faderrow = 20   ; every fader under the pads\n"
      "mixfader3 = 8, center\n"
      "knobrow1 = 30\n"
      "knob2.4 = 31, centre\n"
      "MASTER = 64\n"
      "mixmaster = off\n";
  uint16_t errorLine = 0;
  CHECK(map.load(config, std::strlen(config), &errorLine));
  CHECK(map.assignment(gx::kGroupFader, 0).cc == 20);  // faderrow came after fader1
  CHECK(map.assignment(gx::kGroupFader, 7).cc == 20);
  const gx::CcAssignment& mix3 = map.assignment(gx::kGroupMixFader, 2);
  CHECK(mix3.cc == 8 && mix3.sweep == gx::kSweepCentre);
  CHECK(map.assignment(gx::kGroupMixFader, 3).cc == 7);  // untouched
  CHECK(map.assignment(gx::kGroupKnob, gx::knobIndex(0, 5)).cc == 30);
  const gx::CcAssignment& knob24 = map.assignment(gx::kGroupKnob, gx::knobIndex(1, 3));
  CHECK(knob24.cc == 31 && knob24.sweep == gx::kSweepCentre);
  CHECK(map.assignment(gx::kGroupMasterFader, 0).cc == 64);  // names are case-insensitive
  CHECK(map.assignment(gx::kGroupMixMaster, 0).cc == gx::kNoCc);

  // Bad lines are reported by line number, and what came before them is kept.
  const char* bad = "fader2 = 5\nfader9 = 3\n";
  errorLine = 0;
  CHECK(!map.load(bad, std::strlen(bad), &errorLine) && errorLine == 2);
  CHECK(map.assignment(gx::kGroupFader, 1).cc == 5);
  const char* badCc = "fader2 = 200\n";
  CHECK(!map.load(badCc, std::strlen(badCc), &errorLine) && errorLine == 1);
  const char* badMode = "fader2 = 5, sideways\n";
  CHECK(!map.load(badMode, std::strlen(badMode), &errorLine) && errorLine == 1);
  const char* noEquals = "fader2 5\n";
  CHECK(!map.load(noEquals, std::strlen(noEquals), &errorLine) && errorLine == 1);
  const char* unknown = "slider2 = 5\n";
  CHECK(!map.load(unknown, std::strlen(unknown), &errorLine) && errorLine == 1);

  map.reset();
  CHECK(map.assignment(gx::kGroupFader, 0).cc == 11);
}

void testControlCc() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  f.app.update(0);  // the surface reports every fader and knob once: recorded, not sent
  CHECK(!f.sink.contains(kControl, 0, 7));

  // A mixer fader sends Volume on its strip's track.
  f.sink.events.clear();
  f.surface.moveFader(gx::kGroupMixFader, 2, gx::kFaderMax);
  f.app.update(1);
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kControl, 2, 7));
  CHECK(f.sink.events[0].velocity == 127);

  // The same value again sends nothing; a new value does.
  f.surface.moveFader(gx::kGroupMixFader, 2, gx::kFaderMax);
  f.app.update(2);
  CHECK(f.sink.events.size() == 1);
  f.surface.moveFader(gx::kGroupMixFader, 2, 0);
  f.app.update(3);
  CHECK(f.sink.events.size() == 2 && f.sink.events[1].velocity == 0);

  // A knob on the pan row is centred: the middle sends 64.
  f.sink.events.clear();
  f.surface.moveFader(gx::kGroupKnob, gx::knobIndex(2, 1), gx::kFaderMax / 2 + 10);
  f.app.update(4);
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kControl, 1, 10));
  CHECK(f.sink.events[0].velocity == 64);

  // The faders under the pads send Expression on the same tracks.
  f.sink.events.clear();
  f.surface.moveFader(gx::kGroupFader, 4, 0);
  f.app.update(5);
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kControl, 4, 11));

  // In Global settings fader 1 sets the tempo instead, and sends no CC.
  f.surface.press(gx::kGroupShift, 0);  // Shift + R1
  f.surface.tapButton(gx::kButtonProject);
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(0);  // pick the tempo
  f.sink.events.clear();
  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax / 2);
  f.app.update(6);
  CHECK(f.app.sequencer().bpm() > 150 && !f.sink.contains(kControl, 0, 11));

  // With the track page moved, the strips send on the tracks they now show.
  if (gx::kNumTracks > gx::kTracksPerPage) {
    f.surface.tapButton(gx::kButtonNote);
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 1);
    f.surface.release(gx::kGroupShift, 0);
    f.sink.events.clear();
    f.surface.moveFader(gx::kGroupMixFader, 2, 600);
    f.app.update(7);
    CHECK(f.sink.events.size() == 1 && f.sink.is(0, kControl, 10, 7));
  }

  // An unassigned control sends nothing; assigning it makes it send.
  f.sink.events.clear();
  f.surface.moveFader(gx::kGroupMixMaster, 0, 800);
  f.app.update(8);
  CHECK(f.sink.events.empty());
  const char* config = "mixmaster = 7\n";
  CHECK(f.app.loadControlConfig(config, std::strlen(config)));

  // The same file carries the lines that say where the MIDI ports go. They are not controls,
  // so the map steps over them instead of calling the file wrong - and a line that is neither
  // is still a mistake.
  const char* rig =
      "midiout = MIDI4x4\n"
      "p1 = MIDI4x4:1\n"
      "P8 = Digitone:MIDI 1\n"
      "mixmaster = 7\n";
  CHECK(f.app.loadControlConfig(rig, std::strlen(rig)));
  const char* wrong = "p9 = Nowhere\n";
  uint16_t badLine = 0;
  CHECK(!f.app.loadControlConfig(wrong, std::strlen(wrong), &badLine) && badLine == 1);
  f.surface.moveFader(gx::kGroupMixMaster, 0, 900);
  f.app.update(9);
  // The master faders follow the selected track rather than a strip.
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kControl, f.app.ui().selectedTrack(), 7));

  // A position reported at start-up is recorded, not played.
  f.sink.events.clear();
  f.surface.reportFader(gx::kGroupMixFader, 5, 300);
  f.app.update(10);
  CHECK(f.sink.events.empty());
  f.surface.moveFader(gx::kGroupMixFader, 5, 300);  // the same position: still nothing
  f.app.update(11);
  CHECK(f.sink.events.empty());
  f.surface.moveFader(gx::kGroupMixFader, 5, 700);
  f.app.update(12);
  CHECK(f.sink.events.size() == 1);
}

void testMixer() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  f.app.update(0);
  const gx::UiController& ui = f.app.ui();
  const gx::LedFrame& frame = f.surface.frame;

  // Knob and fader positions reach the core, clamped, and out-of-range indexes are ignored.
  f.surface.moveFader(gx::kGroupKnob, gx::knobIndex(1, 2), 800);
  f.surface.moveFader(gx::kGroupKnob, gx::kNumKnobs, 500);
  f.surface.moveFader(gx::kGroupMixFader, 3, 900);
  f.surface.moveFader(gx::kGroupMixMaster, 0, 5000);
  f.app.update(1);
  CHECK(ui.knobPosition(gx::knobIndex(1, 2)) == 800 && ui.knobPosition(gx::kNumKnobs) == 0);
  CHECK(ui.mixFaderPosition(3) == 900 && ui.mixFaderPosition(gx::kNumMixStrips) == 0);
  CHECK(ui.mixMasterPosition() == gx::kFaderMax);

  // The A2 row picks pages: in Note mode, the pages of steps. The page you are on is green
  // and the row sends no MIDI of its own.
  const size_t eventsBefore = f.sink.events.size();  // the moves above sent their CCs
  const uint8_t a2 = gx::mixButtonIndex(1, 2);
  CHECK(ui.pageKind() == gx::UiController::kPageSteps);
  CHECK(sameColor(frame.mixButtons[gx::mixButtonIndex(1, 0)], gx::kSelectedColor));
  f.surface.tap(gx::kGroupMixButton, a2);
  f.app.update(2);
  CHECK(sameColor(frame.mixButtons[a2], gx::kSelectedColor));  // page 3 now
  CHECK(!sameColor(frame.mixButtons[gx::mixButtonIndex(1, 0)], gx::kSelectedColor));
  CHECK(ui.mode() == gx::kModeNote && ui.selectedTrack() == 0);
  CHECK(f.sink.events.size() == eventsBefore);
  f.surface.tap(gx::kGroupMixButton, gx::mixButtonIndex(1, 0));  // back to page 1
  f.app.update(2);

  // In pattern mode the row mirrors the APC's bottom row, split the same way: the track pages
  // on the left half, the pattern pages on the right. Both surfaces show the same pages.
  f.surface.tapButton(gx::kButtonPattern);
  f.app.update(2);
  const uint8_t a2Patterns = gx::mixButtonIndex(1, kFirstPatternPage);
  f.surface.tap(gx::kGroupMixButton, gx::mixButtonIndex(1, 1));  // A2 2: tracks 9-16
  f.surface.tap(gx::kGroupMixButton, a2Patterns + 1);            // A2 6: patterns 9-16
  f.app.update(2);
  CHECK(sameColor(frame.mixButtons[gx::mixButtonIndex(1, 1)], gx::kSelectedColor));
  CHECK(sameColor(frame.mixButtons[a2Patterns + 1], gx::kSelectedColor));
  CHECK(!sameColor(frame.mixButtons[gx::mixButtonIndex(1, 0)], gx::kSelectedColor));
  CHECK(!sameColor(frame.mixButtons[a2Patterns], gx::kSelectedColor));
  for (uint8_t page = 0; page < gx::kNumBottomButtons; ++page) {  // the APC agrees, button
    const bool onPanel =                                         // for button
        sameColor(frame.mixButtons[gx::mixButtonIndex(1, page)], gx::kSelectedColor);
    CHECK(onPanel == sameColor(frame.bottom[page], gx::kSelectedColor));
  }
  f.surface.tapPad(0);  // the grid moved with them: track 9's pattern 9
  f.app.update(2);
  CHECK(f.app.sequencer().selectedPattern(8) == 8);
  f.surface.tap(gx::kGroupMixButton, gx::mixButtonIndex(1, 0));  // back to tracks 1-8
  f.surface.tap(gx::kGroupMixButton, a2Patterns);                // and patterns 1-8
  f.app.update(2);
  f.surface.tapButton(gx::kButtonProject);
  f.app.update(2);
  CHECK(ui.pageKind() == gx::UiController::kPageProjects);
  f.surface.tap(gx::kGroupMixButton, gx::mixButtonIndex(1, 3));
  f.app.update(2);
  CHECK(f.app.currentProject() / gx::kSlotsPerPage != 3);  // paging doesn't open anything
  f.surface.tapPad(0);                                     // but the pads now show page 4
  f.app.update(2);
  CHECK(f.app.currentProject() == 3 * gx::kSlotsPerPage);
  f.surface.tapButton(gx::kButtonNote);
  f.app.update(2);

  // The side buttons light white while held.
  f.surface.press(gx::kGroupMixSide, 1);
  f.app.update(3);
  CHECK(sameColor(frame.mixSide[1], gx::kWhite));
  f.surface.release(gx::kGroupMixSide, 1);
  f.app.update(4);
  CHECK(gx::isLit(frame.mixSide[1]) && !sameColor(frame.mixSide[1], gx::kWhite));

  // The strips follow the track page: with Shift + B2 strip 1 is track 9.
  if (gx::kNumTracks > gx::kTracksPerPage) {
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 1);
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(5);
    CHECK(ui.selectedTrack() == gx::kTracksPerPage);
    // A surface with bank buttons steps the track page without Shift at all.
    f.app.ui().stepTrackPage(-1);
    f.app.update(5);
    CHECK(ui.selectedTrack() == 0);
  }
}

// Advances a clock in small steps: the engine limits catch-up to a second per call, so a
// single big jump would lose time rather than fast-forward.
template <typename Update>
uint32_t advanceTo(Update update, uint32_t fromMs, uint32_t toMs) {
  for (uint32_t at = fromMs; at < toMs;) {
    at = at + 100 <= toMs ? at + 100 : toMs;
    update(at);
  }
  return toMs;
}

// The transport following the song: each step holds its scene for four bars, holes are
// skipped and the song loops.
void testSongPlayback() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  gx::Sequencer& seq = const_cast<gx::Sequencer&>(f.app.sequencer());
  auto update = [&f](uint32_t ms) { f.app.update(ms); };
  seq.setBpm(120);  // a bar is 2 s, so a song step is 8 s

  // Three sections, each silencing a different track.
  for (uint8_t scene = 0; scene < 3; ++scene) {
    for (uint8_t t = 0; t < 3; ++t) seq.setTrackMuted(t, t == scene);
    seq.captureScene(scene);
  }
  for (uint8_t t = 0; t < 3; ++t) seq.setTrackMuted(t, false);
  // Intro, verse, a hole, drop.
  seq.setSongStep(0, 0);
  seq.setSongStep(1, 1);
  seq.setSongStep(3, 2);

  const uint8_t kFirstSongPad = gx::kNumScenes;
  f.surface.press(gx::kGroupShift, 0);  // Shift + R8: song mode
  f.surface.tapButton(gx::kButtonPlay);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeArrangement && !seq.recording());

  // Play here plays the song, from its first step. The clock starts on the first update
  // after the button, so everything below is measured from there.
  f.surface.tapButton(gx::kButtonPlay);
  const uint32_t t0 = 100;
  uint32_t now = advanceTo(update, 0, t0);
  CHECK(seq.playing() && seq.followingSong());
  CHECK(seq.songPosition() == 0 && seq.currentScene() == 0 && seq.trackMuted(0));
  CHECK(sameColor(frame.pads[kFirstSongPad], gx::kWhite));  // bright: the song is running

  // Four bars later the next step takes over.
  now = advanceTo(update, now, t0 + 7900);
  CHECK(seq.songPosition() == 0 && seq.barPosition() == 3);
  now = advanceTo(update, now, t0 + 8000);
  CHECK(seq.barPosition() == 4 && seq.songPosition() == 1 && seq.currentScene() == 1);
  CHECK(seq.trackMuted(1) && !seq.trackMuted(0));

  // The empty step is skipped, and the song loops round at the end.
  now = advanceTo(update, now, t0 + 16000);
  CHECK(seq.songPosition() == 3 && seq.currentScene() == 2 && seq.trackMuted(2));
  now = advanceTo(update, now, t0 + 24000);
  CHECK(seq.songPosition() == 0 && seq.currentScene() == 0 && seq.trackMuted(0));

  // Tapping a step jumps there, and that section starts its four bars from the jump.
  now = advanceTo(update, now, t0 + 28000);
  f.surface.tapPad(kFirstSongPad + 1);
  now = advanceTo(update, now, t0 + 28100);
  CHECK(seq.songPosition() == 1 && seq.followingSong() && seq.currentScene() == 1);
  now = advanceTo(update, now, t0 + 33900);  // not yet four bars from the jump
  CHECK(seq.songPosition() == 1);
  now = advanceTo(update, now, t0 + 36000);
  CHECK(seq.songPosition() == 3);

  // Launching a scene by hand takes over: the song stops moving, the music carries on.
  f.surface.tapPad(0);  // scene 1 in the palette, queued for the next bar
  now = advanceTo(update, now, t0 + 36100);
  CHECK(!seq.followingSong() && seq.playing());
  CHECK(!sameColor(frame.pads[kFirstSongPad + 3], gx::kWhite));  // only a mark now
  now = advanceTo(update, now, t0 + 40000);
  CHECK(seq.currentScene() == 0 && seq.songPosition() == 3);  // it stayed where it was

  // Play again picks the song up from there; stopping drops it.
  f.surface.tapButton(gx::kButtonPlay);  // stop
  f.app.update(t0 + 40100);
  CHECK(!seq.playing() && !seq.followingSong());
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(t0 + 40200);
  CHECK(seq.playing() && seq.followingSong() && seq.songPosition() == 3);
  CHECK(seq.currentScene() == 2);

  // A song with nothing in it just plays the patterns.
  for (uint8_t step = 0; step < gx::kNumSongSteps; ++step) seq.clearSongStep(step);
  f.surface.tapButton(gx::kButtonPlay);  // stop
  f.app.update(t0 + 40300);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(t0 + 40400);
  CHECK(seq.playing() && !seq.followingSong());
}

// A scene holds which pattern each track plays, not only which tracks are silent, so a
// section changes the music and not just the mix.
void testScenePatterns() {
  RecordingSink sink;
  std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
  gx::Sequencer& seq = *seqOwner;

  // A verse on the first patterns, a chorus on other ones.
  seq.setStepNote(0, 0, 36);
  seq.captureScene(0);
  seq.selectPattern(0, 2);
  seq.selectPattern(1, 5);
  seq.setTrackMuted(2, true);
  seq.captureScene(1);

  // Launching moves the tracks back to the patterns the scene was captured on.
  seq.launchScene(0);
  CHECK(seq.selectedPattern(0) == 0 && seq.selectedPattern(1) == 0 && !seq.trackMuted(2));
  seq.launchScene(1);
  CHECK(seq.selectedPattern(0) == 2 && seq.selectedPattern(1) == 5 && seq.trackMuted(2));

  // Copying a scene copies its patterns too.
  seq.copyScene(1, 7);
  seq.launchScene(0);
  seq.launchScene(7);
  CHECK(seq.selectedPattern(0) == 2 && seq.selectedPattern(1) == 5);

  // They survive a save and reopen (format version 4).
  std::vector<uint8_t> buffer(gx::kMaxEncodedProjectSize);
  const size_t size = gx::encodeProject(seq.project(), &buffer[0], buffer.size());
  CHECK(size > 0);
  static gx::Project reopened;  // megabytes at desktop capacities: too big for the stack
  CHECK(gx::decodeProject(&buffer[0], size, reopened));
  CHECK(sameProject(seq.project(), reopened));
  CHECK(reopened.scenes[1].patterns[0] == 2 && reopened.scenes[1].patterns[1] == 5);

  // A scene saved before scenes remembered patterns leaves them alone: those tracks are
  // stored as kNoPattern, and launching only applies the mutes.
  static gx::Project older;
  older = reopened;
  for (uint8_t t = 0; t < gx::kNumTracks; ++t) older.scenes[1].patterns[t] = gx::kNoPattern;
  seq.setProject(older);
  seq.selectPattern(0, 3);
  seq.launchScene(1);
  CHECK(seq.selectedPattern(0) == 3 && seq.trackMuted(2));
}

// Swing: every second step is held back, without the grid the UI draws moving at all.
void testSwing() {
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    auto tick = [&seq](uint32_t ms) { seq.update(ms); };
    seq.setBpm(120);  // a step is 125 ms, a tick about 5.2 ms
    seq.setStepNote(0, 0, 60);
    seq.setStepNote(0, 1, 62);

    // Straight: the second step plays on its place in the grid.
    CHECK(seq.swing() == gx::kDefaultSwing && gx::kDefaultSwing == gx::kMinSwing);
    seq.play();
    seq.update(0);
    CHECK(sink.contains(kNoteOn, 0, 60));
    sink.events.clear();
    uint32_t now = advanceTo(tick, 0, 130);
    CHECK(sink.contains(kNoteOn, 0, 62));
    seq.stop();

    // Fully swung: it is held back half a step, so it hasn't played by 130 ms.
    seq.setSwing(gx::kMaxSwing);
    sink.events.clear();
    seq.play();
    seq.update(200);
    CHECK(sink.contains(kNoteOn, 0, 60));  // the first of the pair never moves
    sink.events.clear();
    now = advanceTo(tick, 200, 330);
    CHECK(!sink.contains(kNoteOn, 0, 62));
    CHECK(seq.playhead(0) == 1);  // the grid moved on even though the note has not played
    now = advanceTo(tick, now, 395);
    CHECK(sink.contains(kNoteOn, 0, 62));

    // Out of range values are clamped, and the setting is per project.
    seq.setSwing(200);
    CHECK(seq.swing() == gx::kMaxSwing);
    seq.setSwing(0);
    CHECK(seq.swing() == gx::kMinSwing);
    seq.stop();
  }

  // On the surface: swing is the third setting in Global settings, on the same display,
  // fader and up/down pads as the tempo.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  gx::Sequencer& seq = const_cast<gx::Sequencer&>(f.app.sequencer());

  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonProject);  // Shift + R1
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(2);                      // pad 3: swing
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeGlobal);
  CHECK(std::string(f.app.ui().padLabel(2)) == "SWING");

  const uint8_t kUpPad = gx::padIndex(7, 4);
  const uint8_t kDownPad = gx::padIndex(7, 5);
  f.surface.tapPad(kUpPad);
  f.app.update(1);
  CHECK(seq.swing() == gx::kMinSwing + 1 && seq.bpm() == gx::kDefaultBpm);  // not the tempo
  f.surface.tapPad(kDownPad);
  f.app.update(2);
  CHECK(seq.swing() == gx::kMinSwing);
  f.surface.tapPad(kDownPad);  // already at the bottom
  f.app.update(3);
  CHECK(seq.swing() == gx::kMinSwing);

  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax);
  f.app.update(4);
  CHECK(seq.swing() == gx::kMaxSwing && seq.bpm() == gx::kDefaultBpm);
  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax / 2);
  f.app.update(5);
  CHECK(seq.swing() > gx::kMinSwing && seq.swing() < gx::kMaxSwing);

  // It travels with the project.
  const uint8_t saved = seq.swing();
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().swing() == saved);
}

// Micro-timing: a step can play early or late without the grid moving.
void testMicroTiming() {
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    auto tick = [&seq](uint32_t ms) { seq.update(ms); };
    seq.setBpm(120);  // a step is 125 ms, a tick about 5.2 ms
    seq.setStepNote(0, 1, 64);
    CHECK(seq.stepNudge(0, 1) == gx::kDefaultNudge);

    // As early as it goes is twelve ticks, half a step: step 2's place is 125 ms, so it sounds
    // at about 62 ms.
    seq.setStepNudge(0, 1, gx::kMinNudge);
    seq.play();
    seq.update(0);
    uint32_t now = advanceTo(tick, 0, 55);
    CHECK(!sink.contains(kNoteOn, 0, 64));
    now = advanceTo(tick, now, 70);
    CHECK(sink.contains(kNoteOn, 0, 64));  // played before the grid reached step 2
    CHECK(seq.playhead(0) == 0);           // and the grid is still on step 1
    seq.stop();

    // Late: as late as it goes, twelve ticks after its place, about 187 ms.
    sink.events.clear();
    seq.setStepNudge(0, 1, gx::kMaxNudge);
    seq.play();
    seq.update(200);
    now = advanceTo(tick, 200, 375);
    CHECK(!sink.contains(kNoteOn, 0, 64));
    now = advanceTo(tick, now, 395);
    CHECK(sink.contains(kNoteOn, 0, 64));
    seq.stop();

    // Eight ticks is the rung that puts a step on a triplet: a beat is 96 ticks, so the second
    // 8th-note triplet falls at 32, which is step 2's place plus 8. It lands about 167 ms in.
    sink.events.clear();
    seq.setStepNudge(0, 1, 8);
    seq.play();
    seq.update(400);
    now = advanceTo(tick, 400, 564);  // tight enough that +7 would already have sounded
    CHECK(!sink.contains(kNoteOn, 0, 64));
    now = advanceTo(tick, now, 570);
    CHECK(sink.contains(kNoteOn, 0, 64));
    seq.stop();

    // Out of range is clamped, and it belongs to the step.
    seq.setStepNudge(0, 1, 100);
    CHECK(seq.stepNudge(0, 1) == gx::kMaxNudge);
    seq.setStepNudge(0, 1, -100);
    CHECK(seq.stepNudge(0, 1) == gx::kMinNudge);
    CHECK(seq.stepNudge(0, 0) == gx::kDefaultNudge);
  }

  // On the surface: the second lane of Probability mode, straight in the middle.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  gx::Sequencer& seq = const_cast<gx::Sequencer&>(f.app.sequencer());

  f.surface.tapPad(0);  // step 1 on
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonClear);  // Shift + R5: probability
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(0);  // select the step
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeProbability);

  const uint8_t kNudgeRow = 6;  // rows 7 and 8
  f.surface.tapPad(gx::padIndex(kNudgeRow, 0));  // the first pad: as early as it goes
  f.app.update(1);
  CHECK(seq.stepNudge(0, 0) == gx::kMinNudge);
  CHECK(frame.pads[gx::padIndex(kNudgeRow, 0)].r == 255);
  f.surface.tapPad(gx::padIndex(kNudgeRow + 1, 7));  // the last: as late as it goes
  f.app.update(2);
  CHECK(seq.stepNudge(0, 0) == gx::kMaxNudge);
  f.surface.tapPad(gx::padIndex(kNudgeRow + 1, 0));  // straight again
  f.app.update(3);
  CHECK(seq.stepNudge(0, 0) == gx::kDefaultNudge);

  // The lane is a ramp, not a count: single ticks either side of straight, widening outwards,
  // so the pads next to the middle are -1 and +1 and the third pad in is the triplet rung.
  f.surface.tapPad(gx::padIndex(kNudgeRow, 7));
  f.app.update(4);
  CHECK(seq.stepNudge(0, 0) == -1);
  f.surface.tapPad(gx::padIndex(kNudgeRow + 1, 1));
  f.app.update(4);
  CHECK(seq.stepNudge(0, 0) == 1);

  // A value that is not on the ramp - an older project's, from when the lane counted single
  // ticks all the way out - shows on the nearest rung and is left alone until something taps.
  seq.setStepNudge(0, 0, -7);
  f.app.update(4);
  CHECK(seq.stepNudge(0, 0) == -7);                              // still exactly where it was
  CHECK(frame.pads[gx::padIndex(kNudgeRow, 3)].r == 255);        // shown on -6, the nearer rung
  CHECK(frame.pads[gx::padIndex(kNudgeRow, 2)].r == 12);         // not on -8

  // Clear resets it with the rest of the step's parameters, and it is saved.
  f.surface.tapPad(gx::padIndex(kNudgeRow, 2));
  f.app.update(4);
  CHECK(seq.stepNudge(0, 0) == -8);
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().stepNudge(0, 0) == -8);

  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(5);
  CHECK(seq.stepNudge(0, 0) == gx::kDefaultNudge);
}

// The Global settings pad that sends every control's CC again, so the gear catches up with
// the panel. It is a pad rather than a Shift layer because the APC's firmware keeps Shift +
// R6 and R7 for its own pad modes.
void testSendAllControls() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();

  // Move two controls: fader 3 and the mixer's second knob of strip 1.
  f.surface.moveFader(gx::kGroupFader, 2, 600);
  f.surface.moveFader(gx::kGroupKnob, gx::knobIndex(1, 0), 900);
  f.app.update(0);

  f.surface.press(gx::kGroupShift, 0);  // Shift + R1: global settings
  f.surface.tapButton(gx::kButtonProject);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(f.app.ui().mode() == gx::kModeGlobal);
  const uint8_t kSendCcPad = gx::padIndex(7, 7);
  CHECK(std::string(f.app.ui().padLabel(kSendCcPad)) == "SEND\nCC");
  f.sink.events.clear();

  // Both go out again at the same values, and nothing else does — the controls nothing has
  // been heard from stay quiet rather than sending a made-up zero.
  f.surface.tapPad(kSendCcPad);
  f.app.update(2);
  unsigned sent = 0;
  for (size_t i = 0; i < f.sink.events.size(); ++i) {
    if (f.sink.events[i].type == kControl) ++sent;
  }
  CHECK(sent == 2);
  CHECK(f.app.ui().faderPosition(2) == 600);

  // It doesn't disturb the mode or its picked setting.
  CHECK(f.app.ui().mode() == gx::kModeGlobal);
  f.sink.events.clear();
  f.surface.tapPad(kSendCcPad);
  f.app.update(3);
  unsigned again = 0;
  for (size_t i = 0; i < f.sink.events.size(); ++i) {
    if (f.sink.events[i].type == kControl) ++again;
  }
  CHECK(again == 2);  // it resends whether or not anything changed
}

// Bars: the clock everything that happens "in time" hangs off, and scene launches landing on
// the bar rather than the instant a pad is tapped.
void testBarClock() {
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    seq.setBpm(120);  // a step is 125 ms, so a 16-step bar is 2 s
    auto tick = [&seq](uint32_t ms) { seq.update(ms); };

    // Two sections: scene 1 silences track 2, scene 2 silences track 1.
    seq.setTrackMuted(1, true);
    seq.captureScene(0);
    seq.setTrackMuted(1, false);
    seq.setTrackMuted(0, true);
    seq.captureScene(1);
    seq.setTrackMuted(0, false);

    // Stopped, there is no bar to wait for.
    seq.queueScene(0);
    CHECK(seq.pendingScene() == gx::kNoScene && seq.currentScene() == 0 && seq.trackMuted(1));

    seq.play();
    seq.update(0);
    CHECK(seq.stepPosition() == 0 && seq.stepInBar() == 0 && seq.barPosition() == 0);
    uint32_t now = advanceTo(tick, 0, 375);
    CHECK(seq.stepPosition() == 3 && seq.stepInBar() == 3 && seq.barPosition() == 0);

    // Queued in the middle of a bar: nothing moves until the bar turns.
    seq.queueScene(1);
    CHECK(seq.pendingScene() == 1 && seq.currentScene() == 0);
    now = advanceTo(tick, now, 1875);  // the last step of the bar
    CHECK(seq.stepInBar() == 15 && seq.currentScene() == 0);
    CHECK(seq.trackMuted(1) && !seq.trackMuted(0));
    now = advanceTo(tick, now, 2000);  // the top of bar 2
    CHECK(seq.barPosition() == 1 && seq.stepInBar() == 0);
    CHECK(seq.currentScene() == 1 && seq.pendingScene() == gx::kNoScene);
    CHECK(seq.trackMuted(0) && !seq.trackMuted(1));

    // Queueing the scene that is already waiting is how you change your mind.
    seq.queueScene(0);
    CHECK(seq.pendingScene() == 0);
    seq.queueScene(0);
    CHECK(seq.pendingScene() == gx::kNoScene);
    // Queueing another replaces it, and an immediate launch overrides it.
    seq.queueScene(0);
    seq.queueScene(1);
    CHECK(seq.pendingScene() == 1);
    seq.launchScene(0);
    CHECK(seq.pendingScene() == gx::kNoScene && seq.currentScene() == 0);
    // Erasing a scene forgets it, and so does stopping: that bar never comes.
    seq.queueScene(1);
    seq.clearScene(1);
    CHECK(seq.pendingScene() == gx::kNoScene);
    seq.captureScene(1);
    seq.queueScene(1);
    CHECK(seq.pendingScene() == 1);
    seq.stop();
    CHECK(seq.pendingScene() == gx::kNoScene);

    // Bars are counted globally, whatever the tracks' own lengths.
    seq.setTrackLength(0, 12);
    seq.play();
    seq.update(3000);
    now = advanceTo(tick, 3000, 3000 + 125 * 32);
    CHECK(seq.barPosition() == 2 && seq.stepInBar() == 0);
    CHECK(seq.playhead(0) == 32 % 12);
  }

  // On the surface: a tap in scene mode queues, and the pad blinks until it lands.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  gx::Sequencer& seq = const_cast<gx::Sequencer&>(f.app.sequencer());
  auto update = [&f](uint32_t ms) { f.app.update(ms); };
  seq.setBpm(120);

  seq.setTrackMuted(2, true);
  seq.captureScene(0);
  seq.setTrackMuted(2, false);
  seq.captureScene(1);

  f.surface.press(gx::kGroupShift, 0);          // Shift + R2: scene mode
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeScene && seq.playing());

  f.surface.tapPad(0);  // scene 1, early in the bar
  uint32_t now = advanceTo(update, 0, 500);
  CHECK(seq.pendingScene() == 0 && seq.currentScene() == 1 && !seq.trackMuted(2));
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));  // blinking: on at 500 ms
  now = advanceTo(update, now, 750);
  CHECK(sameColor(frame.pads[0], gx::kFilledColor));    // and off at 750 ms
  now = advanceTo(update, now, 2000);                   // the top of the next bar
  CHECK(seq.pendingScene() == gx::kNoScene && seq.currentScene() == 0 && seq.trackMuted(2));
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));  // steady now that it plays
}

void testScenes() {
  {
    // The engine: a scene remembers which tracks were silent and brings them back.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(!seq.sceneUsed(0) && seq.currentScene() == gx::kNoScene);

    seq.setTrackMuted(1, true);
    seq.setTrackMuted(3, true);
    seq.captureScene(0);
    CHECK(seq.sceneUsed(0) && seq.currentScene() == 0);

    // A second section: everything playing.
    seq.setTrackMuted(1, false);
    seq.setTrackMuted(3, false);
    seq.captureScene(1);
    CHECK(seq.sceneUsed(1) && !seq.trackMuted(1));

    // Launching the first brings its mutes back.
    seq.launchScene(0);
    CHECK(seq.trackMuted(1) && seq.trackMuted(3) && !seq.trackMuted(2));
    CHECK(seq.currentScene() == 0);
    seq.launchScene(1);
    CHECK(!seq.trackMuted(1) && !seq.trackMuted(3) && seq.currentScene() == 1);

    // A solo would mask the scene, so launching drops it.
    seq.setTrackSoloed(2, true);
    CHECK(seq.anySolo());
    seq.launchScene(0);
    CHECK(!seq.anySolo() && seq.trackMuted(1));

    // Copy, clear, and the edges.
    seq.copyScene(0, 5);
    CHECK(seq.sceneUsed(5));
    seq.launchScene(5);
    CHECK(seq.trackMuted(1) && seq.trackMuted(3));
    seq.clearScene(5);
    CHECK(!seq.sceneUsed(5) && seq.currentScene() == gx::kNoScene);
    seq.launchScene(7);  // empty: nothing happens
    CHECK(seq.currentScene() == gx::kNoScene);
    seq.captureScene(gx::kNumScenes);  // out of range: ignored
    CHECK(seq.sceneUsed(0));
  }

  // The pads: Shift + R2 opens scene mode, the bottom row mutes, Record + pad captures.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kMuteTrack2 = gx::padIndex(7, 1);

  f.surface.press(gx::kGroupShift, 0);  // Shift + R2
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeScene);
  CHECK(std::string(f.app.ui().padLabel(0)) == "1");
  CHECK(std::string(f.app.ui().padLabel(kMuteTrack2)) == "T2");
  CHECK(sameColor(frame.pads[0], gx::kEmptyColor));           // no scenes yet
  CHECK(sameColor(frame.pads[kMuteTrack2], gx::trackColor(1)));  // track 2 is playing

  // Mute track 2 on the bottom row, then capture that as scene 1.
  f.surface.tapPad(kMuteTrack2);
  f.app.update(1);
  CHECK(seq.trackMuted(1));
  CHECK(!sameColor(frame.pads[kMuteTrack2], gx::trackColor(1)));

  f.surface.press(gx::kGroupRight, gx::kButtonRecord);
  f.app.update(2);
  CHECK(gx::isLit(frame.pads[0]));  // the scene pads turn red: tapping now captures
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonRecord);
  f.app.update(3);
  CHECK(seq.sceneUsed(0) && seq.currentScene() == 0);
  CHECK(!seq.recording());  // R7 was a modifier, so it didn't also start recording
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));

  // A second scene with everything playing, then launch between them.
  f.surface.tapPad(kMuteTrack2);
  f.surface.press(gx::kGroupRight, gx::kButtonRecord);
  f.surface.tapPad(1);
  f.surface.release(gx::kGroupRight, gx::kButtonRecord);
  f.app.update(4);
  CHECK(seq.sceneUsed(1) && !seq.trackMuted(1));

  f.surface.tapPad(0);
  f.app.update(5);
  CHECK(seq.trackMuted(1) && seq.currentScene() == 0);
  CHECK(sameColor(frame.pads[1], gx::kFilledColor));  // the other scene: holds something

  // Clear + pad erases, Duplicate + pad + pad copies.
  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(0);
  f.surface.tapPad(9);
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.app.update(6);
  CHECK(seq.sceneUsed(9));
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(9);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(7);
  CHECK(!seq.sceneUsed(9));

  // R7 on its own still records, and R2 goes back to pattern mode.
  f.surface.tapButton(gx::kButtonRecord);
  f.app.update(8);
  CHECK(seq.recording());
  f.surface.tapButton(gx::kButtonPattern);
  f.app.update(9);
  CHECK(f.app.ui().mode() == gx::kModePattern);

  // Shift + R2 opens scenes from pattern mode as it does anywhere else, and R2 goes back:
  // pattern mode has no Shift function of its own to get in the way.
  tapWithShift(f.surface, gx::kButtonPattern);
  f.app.update(10);
  CHECK(f.app.ui().mode() == gx::kModeScene);
  f.surface.tapButton(gx::kButtonPattern);
  f.app.update(11);
  CHECK(f.app.ui().mode() == gx::kModePattern);

  // The mutes stay put while you go and try patterns: they belong to the session, not to a
  // mode. That is what makes flipping between the two to audition a pattern work.
  const bool mutedBefore = seq.trackMuted(1);
  CHECK(mutedBefore);

  // In pattern mode a muted track's whole column is dimmer than the same pad on a track that
  // plays, so it is clear why nothing is coming out of it.
  f.surface.tapPad(gx::padIndex(0, 0));  // track 1 pattern 1, so both columns show the same
  f.app.update(11);
  const gx::Rgb playing = frame.pads[gx::padIndex(1, 0)];
  const gx::Rgb silent = frame.pads[gx::padIndex(1, 1)];
  CHECK(gx::isLit(playing) && !sameColor(playing, silent));

  // Shift + the bottom row mutes a track from here, and the row shows the mutes while held.
  f.surface.press(gx::kGroupShift, 0);
  f.app.update(12);
  CHECK(sameColor(frame.pads[gx::padIndex(7, 0)], gx::trackColor(0)));   // track 1 plays
  CHECK(!sameColor(frame.pads[gx::padIndex(7, 1)], gx::trackColor(1)));  // track 2 is muted
  f.surface.tapPad(gx::padIndex(7, 0));  // Shift + bottom row: mute track 1
  f.surface.tapPad(gx::padIndex(7, 1));  // and unmute track 2
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(13);
  CHECK(seq.trackMuted(0) && !seq.trackMuted(1));
  CHECK(seq.selectedPattern(0) == 0);  // the pads under Shift didn't also pick a pattern
  f.surface.tapPad(gx::padIndex(7, 0));  // without Shift the same pad picks pattern 8
  f.app.update(14);
  CHECK(seq.selectedPattern(0) == 7 && seq.trackMuted(0));

  // Put it back the way the scene had it.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapPad(gx::padIndex(7, 0));
  f.surface.tapPad(gx::padIndex(7, 1));
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(15);
  CHECK(!seq.trackMuted(0) && seq.trackMuted(1));
  f.surface.tapPad(gx::padIndex(1, 0));  // track 1 plays its pattern 2
  f.app.update(12);
  CHECK(seq.selectedPattern(0) == 1 && seq.trackMuted(1) == mutedBefore);
  tapWithShift(f.surface, gx::kButtonPattern);  // back to scenes
  f.app.update(13);
  CHECK(f.app.ui().mode() == gx::kModeScene && seq.trackMuted(1) == mutedBefore);
  CHECK(seq.currentScene() == 0);  // and the scene showing is still the one you launched


  // Scenes are saved with the project.
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().sceneUsed(0) && reopened->app.sequencer().sceneUsed(1));
}

void testArrangement() {
  {
    // The engine: the song is scenes in order, one scene per step.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(seq.songStep(0) == gx::kNoScene && seq.songPosition() == gx::kNoSongStep);

    seq.setTrackMuted(1, true);
    seq.captureScene(0);           // an intro with track 2 out
    seq.setTrackMuted(1, false);
    seq.captureScene(1);           // and a drop with everything in

    seq.setSongStep(0, 0);
    seq.setSongStep(1, 1);
    seq.setSongStep(2, 1);         // the same scene twice: a longer section
    CHECK(seq.songStep(0) == 0 && seq.songStep(2) == 1);
    seq.setSongStep(3, 7);         // an empty scene can't go in the song
    CHECK(seq.songStep(3) == gx::kNoScene);
    seq.setSongStep(gx::kNumSongSteps, 0);  // out of range: ignored

    // Going to a step plays its scene.
    seq.goToSongStep(1);
    CHECK(seq.songPosition() == 1 && seq.currentScene() == 1 && !seq.trackMuted(1));
    seq.goToSongStep(0);
    CHECK(seq.songPosition() == 0 && seq.trackMuted(1));
    seq.goToSongStep(9);  // an empty step is not somewhere to be
    CHECK(seq.songPosition() == 0);

    seq.copySongStep(0, 4);
    CHECK(seq.songStep(4) == 0);
    seq.clearSongStep(0);
    CHECK(seq.songStep(0) == gx::kNoScene && seq.songPosition() == gx::kNoSongStep);
  }

  // The pads: scenes on top, the song below.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kFirstSongPad = gx::kNumScenes;

  // Two scenes to arrange, captured in scene mode.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonPattern);  // Shift + R2: scenes
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(gx::padIndex(7, 1));     // mute track 2
  f.surface.press(gx::kGroupRight, gx::kButtonRecord);
  f.surface.tapPad(0);                      // capture scene 1
  f.surface.release(gx::kGroupRight, gx::kButtonRecord);
  f.surface.tapPad(gx::padIndex(7, 1));     // unmute
  f.surface.press(gx::kGroupRight, gx::kButtonRecord);
  f.surface.tapPad(1);                      // capture scene 2
  f.surface.release(gx::kGroupRight, gx::kButtonRecord);
  f.app.update(0);
  CHECK(seq.sceneUsed(0) && seq.sceneUsed(1));

  // Shift + R8 opens the song — R8 alone is Play, and the APC's firmware keeps R6 and R7.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonPlay);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(f.app.ui().mode() == gx::kModeArrangement);
  CHECK(!seq.playing());  // Shift + R8 opened a mode, it didn't start the transport

  // Hold a song step and tap a scene to put it there.
  f.surface.press(gx::kGroupPad, kFirstSongPad);
  f.surface.tapPad(0);  // scene 1
  f.surface.release(gx::kGroupPad, kFirstSongPad);
  f.app.update(2);
  CHECK(seq.songStep(0) == 0);
  CHECK(seq.songPosition() == gx::kNoSongStep);  // choosing a scene isn't going there
  CHECK(std::string(f.app.ui().padLabel(kFirstSongPad)) == "S1");

  f.surface.press(gx::kGroupPad, kFirstSongPad + 1);
  f.surface.tapPad(1);  // scene 2
  f.surface.release(gx::kGroupPad, kFirstSongPad + 1);
  f.app.update(3);
  CHECK(seq.songStep(1) == 1);

  // Tapping a step on its own goes there and plays its scene.
  f.surface.tapPad(kFirstSongPad);
  f.app.update(4);
  CHECK(seq.songPosition() == 0 && seq.currentScene() == 0 && seq.trackMuted(1));
  // Where the song stands: white, but dimmed, because the song is not running itself.
  CHECK(gx::isLit(frame.pads[kFirstSongPad]) && !sameColor(frame.pads[kFirstSongPad], gx::kWhite));
  CHECK(!seq.followingSong());
  CHECK(gx::isLit(frame.pads[kFirstSongPad + 1]));          // the other step shows its scene

  // Holding a step shows which scene it plays, brighter than the rest of the palette.
  f.surface.press(gx::kGroupPad, kFirstSongPad + 1);
  f.app.update(5);
  CHECK(gx::isLit(frame.pads[1]) && !sameColor(frame.pads[1], gx::kFilledColor));
  f.surface.release(gx::kGroupPad, kFirstSongPad + 1);
  f.app.update(6);
  CHECK(seq.songPosition() == 1);  // released without a scene: it went there

  // Duplicate repeats a section, Clear leaves a hole.
  f.surface.press(gx::kGroupRight, gx::kButtonDuplicate);
  f.surface.tapPad(kFirstSongPad + 1);
  f.surface.tapPad(kFirstSongPad + 2);
  f.surface.release(gx::kGroupRight, gx::kButtonDuplicate);
  f.app.update(7);
  CHECK(seq.songStep(2) == 1);
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(kFirstSongPad + 2);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(8);
  CHECK(seq.songStep(2) == gx::kNoScene);

  // A scene tapped on its own still just plays, and the song is saved with the project.
  f.surface.tapPad(1);
  f.app.update(9);
  CHECK(seq.currentScene() == 1 && !seq.trackMuted(1));
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().songStep(0) == 0);
  CHECK(reopened->app.sequencer().songStep(1) == 1);
}

void testMuteSolo() {
  {
    // The engine: a muted track's steps don't play, and what it is playing stops at once.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    seq.setStepNote(0, 0, 36);
    seq.setStepNote(1, 0, 48);
    seq.setTrackLength(0, 2);  // 2-step patterns, so step 1 comes round every 250 ms
    seq.setTrackLength(1, 2);
    seq.setStepGate(0, 0, 4 * gx::kTicksPerStep);  // still sounding when it is muted
    seq.play();
    seq.update(0);
    CHECK(sink.contains(kNoteOn, 0, 36) && sink.contains(kNoteOn, 1, 48));

    sink.events.clear();
    seq.setTrackMuted(0, true);
    CHECK(seq.trackMuted(0) && !seq.trackAudible(0));
    CHECK(sink.is(0, kNoteOff, 0, 36) && sink.events.size() == 1);
    sink.events.clear();
    seq.update(125);  // step 2, then back round to step 1
    seq.update(250);
    CHECK(!sink.contains(kNoteOn, 0, 36));

    // Unmuting brings it back at the next step.
    seq.setTrackMuted(0, false);
    sink.events.clear();
    seq.update(375);
    seq.update(500);
    CHECK(sink.contains(kNoteOn, 0, 36));

    // Solo silences every track that isn't soloed; dropping it brings them back. Track 1's
    // note is still sounding on its long gate, so soloing track 2 releases it.
    sink.events.clear();
    seq.setTrackSoloed(1, true);
    CHECK(seq.anySolo() && !seq.trackAudible(0) && seq.trackAudible(1));
    CHECK(sink.contains(kNoteOff, 0, 36));
    sink.events.clear();
    seq.update(625);
    seq.update(750);
    CHECK(sink.contains(kNoteOn, 1, 48) && !sink.contains(kNoteOn, 0, 36));
    seq.setTrackSoloed(1, false);
    CHECK(!seq.anySolo() && seq.trackAudible(0));

    // A muted track stays muted through a solo.
    seq.setTrackMuted(1, true);
    seq.setTrackSoloed(0, true);
    CHECK(seq.trackAudible(0) && !seq.trackAudible(1));
    seq.setTrackSoloed(0, false);
    CHECK(!seq.trackAudible(1) && seq.trackMuted(1));
  }

  // The mixer: a mute button per strip, and the mixer's Shift (M4) makes it solo.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  f.app.update(0);
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t mute0 = gx::mixButtonIndex(0, 0);
  const uint8_t mute2 = gx::mixButtonIndex(0, 2);

  // The button lights when its track is NOT heard: dark while it plays, its colour when muted.
  CHECK(!sameColor(frame.mixButtons[mute2], gx::trackColor(2)));
  const gx::Rgb playing = frame.mixButtons[mute2];
  f.surface.tap(gx::kGroupMixButton, mute2);
  f.app.update(1);
  CHECK(seq.trackMuted(2) && sameColor(frame.mixButtons[mute2], gx::trackColor(2)));
  f.surface.tap(gx::kGroupMixButton, mute2);
  f.app.update(2);
  CHECK(!seq.trackMuted(2) && sameColor(frame.mixButtons[mute2], playing));

  // Holding the mixer's Shift makes the same button solo: the soloed track goes dark, since
  // it is the one you hear, and every other button lights.
  f.surface.press(gx::kGroupMixSide, gx::kMixShiftButton);
  f.surface.tap(gx::kGroupMixButton, mute0);
  f.app.update(3);
  CHECK(seq.trackSoloed(0) && !seq.trackMuted(0));
  CHECK(!sameColor(frame.mixButtons[mute0], gx::trackColor(0)));
  CHECK(brightness(frame.mixButtons[mute0]) < brightness(frame.mixButtons[mute2]));
  CHECK(gx::isLit(frame.mixButtons[mute2]));  // track 3 is silent now, so it lights
  f.surface.release(gx::kGroupMixSide, gx::kMixShiftButton);

  // A muted track and one the solo is silencing both light, but not identically.
  const uint8_t mute4 = gx::mixButtonIndex(0, 4);
  f.surface.tap(gx::kGroupMixButton, mute4);  // Shift is released, so this mutes track 5
  f.app.update(4);
  CHECK(seq.trackMuted(4) && !seq.trackSoloed(4));
  CHECK(gx::isLit(frame.mixButtons[mute4]) && gx::isLit(frame.mixButtons[mute2]));
  CHECK(!sameColor(frame.mixButtons[mute4], frame.mixButtons[mute2]));
  f.surface.tap(gx::kGroupMixButton, mute4);

  // Soloing the same track again drops the solo and brings the others back: every button
  // goes dark, which is how you get back to hearing everything.
  f.surface.press(gx::kGroupMixSide, gx::kMixShiftButton);
  f.surface.tap(gx::kGroupMixButton, mute0);
  f.surface.release(gx::kGroupMixSide, gx::kMixShiftButton);
  f.app.update(5);
  CHECK(!seq.trackSoloed(0) && !seq.anySolo() && !seq.trackMuted(4));
  CHECK(sameColor(frame.mixButtons[mute2], playing));
  for (uint8_t strip = 0; strip < gx::kNumMixStrips; ++strip) {
    CHECK(brightness(frame.mixButtons[gx::mixButtonIndex(0, strip)]) < 64);
  }
}

bool padLit(const gx::LedFrame& frame, uint8_t row, uint8_t col) {
  return gx::isLit(frame.pads[gx::padIndex(row, col)]);
}

void testGlobalMode() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const gx::Rgb kAmber = {255, 140, 0};

  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonProject);  // Shift + R1
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeGlobal);
  CHECK(sameColor(frame.right[gx::kButtonProject], gx::kWhite));
  CHECK(gx::isLit(frame.pads[0]) && !sameColor(frame.pads[0], gx::kSelectedColor));

  // Until a setting is picked, fader 1 does nothing and no value is shown.
  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax);
  f.app.update(1);
  CHECK(seq.bpm() == gx::kDefaultBpm);
  CHECK(!padLit(frame, 2, 6));
  const uint8_t kUpPad = gx::padIndex(7, 4);  // row 8, column 5: +1 BPM
  const uint8_t kDownPad = gx::padIndex(7, 5);  // row 8, column 6: -1 BPM
  f.surface.tapPad(kUpPad);
  f.app.update(1);
  CHECK(seq.bpm() == gx::kDefaultBpm && !gx::isLit(frame.pads[kUpPad]));

  // Pad 1 picks the tempo. 120 shows as a narrow 1, an amber 2 and a white 0.
  f.surface.tapPad(0);
  f.app.update(2);
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));
  for (uint8_t row = 2; row <= 6; ++row) CHECK(padLit(frame, row, 1));  // the 1's stem
  CHECK(padLit(frame, 3, 0) && !padLit(frame, 2, 0) && !padLit(frame, 6, 0));
  CHECK(sameColor(frame.pads[gx::padIndex(2, 2)], kAmber));  // top bar of the 2
  CHECK(!padLit(frame, 3, 2) && padLit(frame, 3, 4) && padLit(frame, 5, 2));
  CHECK(sameColor(frame.pads[gx::padIndex(2, 6)], gx::kWhite));  // top bar of the 0
  CHECK(!padLit(frame, 4, 6) && padLit(frame, 4, 5) && padLit(frame, 4, 7));
  CHECK(!padLit(frame, 1, 0) && !padLit(frame, 7, 0));

  // The up and down pads step the tempo by 1 and light white while held.
  CHECK(gx::isLit(frame.pads[kUpPad]) && gx::isLit(frame.pads[kDownPad]));
  f.surface.tapPad(kUpPad);
  f.app.update(2);
  CHECK(seq.bpm() == 121);
  f.surface.tapPad(kDownPad);
  f.surface.tapPad(kDownPad);
  f.app.update(2);
  CHECK(seq.bpm() == 119);
  f.surface.press(gx::kGroupPad, kUpPad);
  f.app.update(2);
  CHECK(seq.bpm() == 120 && sameColor(frame.pads[kUpPad], gx::kWhite));
  f.surface.release(gx::kGroupPad, kUpPad);
  f.app.update(2);
  CHECK(!sameColor(frame.pads[kUpPad], gx::kWhite));
  CHECK(padLit(frame, 3, 0) && !padLit(frame, 2, 0));  // 120 again: narrow 1

  // Fader 1 covers the whole tempo range, and the display follows.
  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax);
  f.app.update(3);
  CHECK(seq.bpm() == gx::kMaxBpm);
  CHECK(padLit(frame, 2, 0) && !padLit(frame, 3, 0) && padLit(frame, 4, 0));  // narrow 3
  f.surface.tapPad(kUpPad);  // already at the top: up does nothing and is dimmed
  f.app.update(3);
  CHECK(seq.bpm() == gx::kMaxBpm);
  CHECK(frame.pads[kUpPad].r < frame.pads[kDownPad].r);
  f.surface.moveFader(gx::kGroupFader, 0, 0);
  f.app.update(4);
  CHECK(seq.bpm() == gx::kMinBpm);
  for (uint8_t row = 2; row <= 6; ++row) CHECK(!padLit(frame, row, 0) && !padLit(frame, row, 1));
  f.surface.tapPad(kDownPad);  // already at the bottom
  f.app.update(4);
  CHECK(seq.bpm() == gx::kMinBpm && frame.pads[kDownPad].r < frame.pads[kUpPad].r);
  f.surface.moveFader(gx::kGroupFader, 0, 512);
  f.app.update(5);
  CHECK(seq.bpm() == 160);
  f.surface.moveFader(gx::kGroupFader, 1, 0);  // other faders don't set it
  f.app.update(6);
  CHECK(seq.bpm() == 160);

  // R1 goes to project mode, where fader 1 leaves the tempo alone.
  f.surface.tapButton(gx::kButtonProject);
  f.app.update(7);
  CHECK(f.app.ui().mode() == gx::kModeProject);
  f.surface.moveFader(gx::kGroupFader, 0, gx::kFaderMax);
  f.app.update(8);
  CHECK(seq.bpm() == 160);

  // The tempo is still picked on the next visit.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonProject);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(9);
  CHECK(f.app.ui().mode() == gx::kModeGlobal && sameColor(frame.pads[0], gx::kSelectedColor));

  // Shift + R1 opens global settings from pattern mode too.
  f.surface.tapButton(gx::kButtonPattern);
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonProject);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(10);
  CHECK(f.app.ui().mode() == gx::kModeGlobal);
}

// The note of the last Note On the sink received.
uint16_t lastNoteOn(const RecordingSink& sink) {
  for (size_t i = sink.events.size(); i > 0; --i) {
    if (sink.events[i - 1].type == kNoteOn) return sink.events[i - 1].value;
  }
  return 0xFFFF;
}

// Taps a keyboard pad and returns the note it played.
uint16_t playKey(Fixture& f, uint8_t pad, uint32_t nowMs) {
  f.surface.tapPad(pad);
  f.app.update(nowMs);
  return lastNoteOn(f.sink);
}

void testKeyboardLayouts() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kDrumPad = gx::padIndex(7, 6);
  const uint8_t kOwnScalePad = gx::padIndex(7, 7);
  const uint8_t kFirstScalePad = 4 * gx::kGridCols;
  const uint8_t kMajorPad = kFirstScalePad + gx::kScaleMajor;
  const uint8_t kMinorPad = kFirstScalePad + gx::kScaleMinor;
  const uint8_t kChromaticPad = kFirstScalePad + gx::kScaleChromatic;
  const uint8_t kRootAPad = gx::padIndex(1, 5);
  const uint8_t kRootCPad = gx::padIndex(1, 0);
  const uint8_t kBottomLeftKey = gx::padIndex(7, 0);
  const gx::Rgb kTrack1 = gx::trackColor(0);

  // The song is in A minor, and both layout pads are dim.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kRootAPad);
  f.surface.tapPad(kMinorPad);
  f.app.update(0);
  CHECK(seq.scaleRoot() == 9 && seq.scale() == gx::kScaleMinor);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardProjectScale);
  CHECK(sameColor(frame.pads[kMinorPad], gx::kSelectedColor));
  CHECK(gx::isLit(frame.pads[kOwnScalePad]) && !sameColor(frame.pads[kOwnScalePad], kTrack1));
  CHECK(gx::isLit(frame.pads[kDrumPad]) && !sameColor(frame.pads[kDrumPad], kTrack1));

  // Following the song: track 1's keyboard starts on A2, then B2.
  f.surface.tapButton(gx::kButtonNote);
  CHECK(playKey(f, kBottomLeftKey, 1) == 45);
  CHECK(playKey(f, gx::padIndex(7, 1), 1) == 47);

  // An own scale starts as a copy of the song's key, shown in the track colour.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kOwnScalePad);
  f.app.update(2);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardOwnScale);
  CHECK(seq.keyboardRoot(0) == 9 && seq.keyboardScale(0) == gx::kScaleMinor);
  CHECK(sameColor(frame.pads[kOwnScalePad], kTrack1));
  CHECK(sameColor(frame.pads[kMinorPad], kTrack1) && sameColor(frame.pads[kRootAPad], kTrack1));
  CHECK(sameColor(frame.pads[kMajorPad], gx::kFilledColor));  // the other scales stay blue

  // Picks now change only track 1: C chromatic. The song stays in A minor.
  f.surface.tapPad(kRootCPad);
  f.surface.tapPad(kChromaticPad);
  f.app.update(3);
  CHECK(seq.keyboardRoot(0) == 0 && seq.keyboardScale(0) == gx::kScaleChromatic);
  CHECK(seq.scaleRoot() == 9 && seq.scale() == gx::kScaleMinor);
  CHECK(sameColor(frame.pads[kChromaticPad], kTrack1) && sameColor(frame.pads[kRootCPad], kTrack1));
  const gx::Rgb hint = frame.pads[kMinorPad];  // the song's A minor, as a faint green hint
  CHECK(hint.g > hint.r && hint.g > hint.b);
  CHECK(!sameColor(hint, gx::kSelectedColor) && !sameColor(hint, gx::kFilledColor));
  CHECK(sameColor(frame.pads[kRootAPad], hint));
  f.surface.tapButton(gx::kButtonNote);
  CHECK(playKey(f, kBottomLeftKey, 3) == 36);
  CHECK(playKey(f, gx::padIndex(7, 1), 3) == 37);

  // Track 2 follows the song, so its picks change the song's key: A major.
  f.surface.tap(gx::kGroupBottom, 1);
  CHECK(playKey(f, kBottomLeftKey, 4) == 45);
  tapWithShift(f.surface, gx::kButtonNote);
  f.app.update(5);
  CHECK(sameColor(frame.pads[kMinorPad], gx::kSelectedColor));
  f.surface.tapPad(kMajorPad);
  f.app.update(5);
  CHECK(seq.scale() == gx::kScaleMajor);
  CHECK(seq.keyboardScale(0) == gx::kScaleChromatic);  // track 1 keeps its own

  // Turning track 1's own scale off follows the song; turning it on again copies the song's
  // key afresh, now A major.
  f.surface.tap(gx::kGroupBottom, 0);
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kOwnScalePad);
  f.app.update(6);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardProjectScale);
  CHECK(seq.keyboardScale(0) == gx::kScaleMajor);
  f.surface.tapPad(kOwnScalePad);
  f.app.update(6);
  CHECK(seq.keyboardRoot(0) == 9 && seq.keyboardScale(0) == gx::kScaleMajor);

  // Drums replace the own scale: two 4x4 blocks of consecutive notes rising left to right and
  // bottom to top, 36-51 on the left and 52-67 on the right.
  f.surface.tapPad(kDrumPad);
  f.app.update(7);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardDrums);
  CHECK(frame.pads[kDrumPad].r == 255 && frame.pads[kDrumPad].g == 150 &&
        frame.pads[kDrumPad].b == 0);  // amber
  CHECK(!sameColor(frame.pads[kOwnScalePad], kTrack1));
  CHECK(seq.project().tracks[0].scale == gx::kNoOwnScale);  // the own scale was dropped
  CHECK(sameColor(frame.pads[kMajorPad], gx::kSelectedColor));  // picks set the song key again
  f.surface.tapButton(gx::kButtonNote);
  f.app.update(8);
  CHECK(!sameColor(frame.pads[gx::padIndex(7, 3)], frame.pads[gx::padIndex(7, 4)]));
  CHECK(sameColor(frame.pads[gx::padIndex(4, 0)], frame.pads[gx::padIndex(7, 3)]));
  CHECK(playKey(f, kBottomLeftKey, 8) == 36);
  CHECK(playKey(f, gx::padIndex(7, 3), 8) == 39);
  CHECK(playKey(f, gx::padIndex(6, 0), 8) == 40);
  CHECK(playKey(f, gx::padIndex(4, 3), 8) == 51);
  CHECK(playKey(f, gx::padIndex(7, 4), 8) == 52);
  CHECK(playKey(f, gx::padIndex(4, 7), 8) == 67);

  // Holding drum pads and pressing a step writes them as a chord: closed hat and kick.
  f.surface.press(gx::kGroupPad, kBottomLeftKey);
  f.surface.press(gx::kGroupPad, gx::padIndex(6, 2));  // 42
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupPad, kBottomLeftKey);
  f.surface.release(gx::kGroupPad, gx::padIndex(6, 2));
  f.app.update(9);
  CHECK(seq.stepActive(0, 0) && seq.stepNoteCount(0, 0) == 2);
  CHECK(seq.stepNote(0, 0, 0) == 42 && seq.stepNote(0, 0, 1) == 36);

  // Tapping Drums again follows the song; Own scale then starts from the song's key again.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kDrumPad);
  f.app.update(10);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardProjectScale);
  f.surface.tapPad(kOwnScalePad);
  f.app.update(10);
  CHECK(seq.keyboardRoot(0) == 9 && seq.keyboardScale(0) == gx::kScaleMajor);

  // Worth saving even with no steps: a drum track, or a track with its own scale.
  static gx::Project project;
  gx::initProject(project);
  CHECK(gx::isProjectEmpty(project));
  project.tracks[4].keyboardLayout = gx::kKeyboardDrums;
  CHECK(!gx::isProjectEmpty(project));
  gx::initProject(project);
  project.tracks[5].keyboardLayout = gx::kKeyboardOwnScale;
  project.tracks[5].scale = gx::kScaleMinor;
  CHECK(!gx::isProjectEmpty(project));
}

void testPianoRoll() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kRollPad = gx::padIndex(7, 0);
  const gx::Rgb kTrack1 = gx::trackColor(0);

  // The song is in C major; track 1 turns on its piano roll in scale mode.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(4 * gx::kGridCols + gx::kScaleMajor);
  f.app.update(0);
  CHECK(gx::isLit(frame.pads[kRollPad]) && !sameColor(frame.pads[kRollPad], gx::kWhite));
  f.surface.tapPad(kRollPad);
  f.app.update(0);
  CHECK(seq.trackPianoRoll(0) && !seq.trackPianoRoll(1));
  CHECK(sameColor(frame.pads[kRollPad], gx::kWhite));
  f.surface.tapButton(gx::kButtonNote);

  // Columns are steps 1-8; rows are C major from C2 at the bottom to C3 at the top. A tapped
  // note joins the step's chord and plays.
  f.surface.tapPad(gx::padIndex(7, 0));  // C2, step 1
  f.app.update(1);
  CHECK(seq.stepActive(0, 0) && seq.stepNoteCount(0, 0) == 1 && seq.stepNote(0, 0) == 36);
  CHECK(lastNoteOn(f.sink) == 36);
  f.surface.tapPad(gx::padIndex(6, 0));  // D2 joins it
  f.surface.tapPad(gx::padIndex(0, 3));  // C3, step 4
  f.app.update(2);
  CHECK(seq.stepNoteCount(0, 0) == 2 && seq.stepNote(0, 0, 1) == 38);
  CHECK(seq.stepActive(0, 3) && seq.stepNote(0, 3) == 48);
  CHECK(sameColor(frame.pads[gx::padIndex(7, 0)], kTrack1));
  CHECK(sameColor(frame.pads[gx::padIndex(6, 0)], kTrack1));
  CHECK(!sameColor(frame.pads[gx::padIndex(5, 0)], kTrack1));  // E2 isn't in the chord

  // Tapping a note again takes it out; the step switches off with its last note.
  f.surface.tapPad(gx::padIndex(6, 0));
  f.surface.tapPad(gx::padIndex(7, 0));
  f.app.update(3);
  CHECK(!seq.stepActive(0, 0) && seq.stepNoteCount(0, 0) == 0);

  // Shift + a bottom-row pad makes its step the last. While Shift is held, B5-B8 are the
  // scroll buttons, lit where there's further to go: nothing left of step 1.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapPad(gx::padIndex(7, 5));  // step 6
  f.app.update(4);
  CHECK(seq.trackLength(0) == 6);
  CHECK(gx::isLit(frame.bottom[4]) && gx::isLit(frame.bottom[5]));
  CHECK(!gx::isLit(frame.bottom[6]) && gx::isLit(frame.bottom[7]));

  // Shift + B5 scrolls the notes up 4 (the bottom row is now G2); Shift + B8 the steps right 4.
  f.surface.tap(gx::kGroupBottom, 4);
  f.surface.tap(gx::kGroupBottom, 7);
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(gx::padIndex(7, 0));  // G2, step 5
  f.app.update(5);
  CHECK(seq.stepActive(0, 4) && seq.stepNote(0, 4) == 43);
  CHECK(sameColor(frame.pads[gx::padIndex(7, 3)], gx::kBlack));  // step 8 is past the end
  CHECK(frame.pads[gx::padIndex(6, 1)].r > frame.pads[gx::padIndex(6, 1)].g);  // step 6: last

  // Hold R3 + B2 to jump to the second step page.
  f.surface.press(gx::kGroupRight, gx::kButtonNote);
  f.surface.tap(gx::kGroupBottom, 1);
  f.surface.release(gx::kGroupRight, gx::kButtonNote);
  f.surface.tapPad(gx::padIndex(7, 0));
  f.app.update(6);
  CHECK(seq.stepActive(0, gx::kStepsPerPage) && seq.stepNote(0, gx::kStepsPerPage) == 43);

  // Shift + B1..B4 still pick track pages.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 1);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(7);
  CHECK(f.app.ui().selectedTrack() == gx::kTracksPerPage);

  // Turning the piano roll off brings back the steps and keyboard.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tap(gx::kGroupBottom, 0);
  f.surface.release(gx::kGroupShift, 0);
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kRollPad);
  f.surface.tapButton(gx::kButtonNote);
  f.app.update(8);
  CHECK(f.app.ui().selectedTrack() == 0 && !seq.trackPianoRoll(0));

  // A track showing a piano roll is worth saving even with no steps.
  static gx::Project project;
  gx::initProject(project);
  project.tracks[2].pianoRoll = 1;
  CHECK(!gx::isProjectEmpty(project));
}

// A MIDI connection that records what is sent and plays back queued bytes.
class FakeMidiPort : public gx::MidiPort {
 public:
  size_t read(uint8_t* buffer, size_t capacity) override {
    const size_t count = std::min(capacity, incoming.size());
    std::copy(incoming.begin(), incoming.begin() + count, buffer);
    incoming.erase(incoming.begin(), incoming.begin() + count);
    return count;
  }
  bool write(const uint8_t* data, size_t size) override {
    sent.insert(sent.end(), data, data + size);
    return true;
  }
  void receive(std::initializer_list<uint8_t> bytes) { incoming.insert(incoming.end(), bytes); }

  std::vector<uint8_t> incoming;
  std::vector<uint8_t> sent;
};

bool containsBytes(const std::vector<uint8_t>& data, std::initializer_list<uint8_t> bytes) {
  return std::search(data.begin(), data.end(), bytes.begin(), bytes.end()) != data.end();
}

void testApcMiniSurface() {
  FakeMidiPort port;
  gx::ControlEvent e;
  {
    gx::ApcMiniSurface apc(port);

    // begin() sends the Introduction message.
    CHECK(apc.begin());
    const std::vector<uint8_t> intro = {0xF0, 0x47, 0x7F, 0x4F, 0x60, 0x00,
                                        0x04, 0x00, 0x01, 0x00, 0x00, 0xF7};
    CHECK(port.sent == intro);
    CHECK(!apc.pollEvent(e));

    // Note 0 is the bottom-left pad, our row 8 column 1; note 63 our top-right pad.
    port.receive({0x90, 0x00, 0x7F, 0x80, 0x00, 0x00, 0x90, 0x3F, 0x7F});
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupPad && e.index == gx::padIndex(7, 0) && e.pressed);
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupPad && e.index == gx::padIndex(7, 0) && !e.pressed);
    CHECK(apc.pollEvent(e) && e.index == gx::padIndex(0, 7) && e.pressed);
    // Running status, and a Note On with velocity 0, which is a release.
    port.receive({0x3F, 0x00});
    CHECK(apc.pollEvent(e) && e.index == gx::padIndex(0, 7) && !e.pressed);

    // Note 112 is the top right-hand button, R1; 107 is B8; 122 is Shift.
    port.receive({0x90, 112, 127, 0x90, 107, 127, 0x90, 122, 127});
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupRight && e.index == 0 && e.pressed);
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupBottom && e.index == 7);
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupShift && e.pressed);

    // CC 48 is fader 1 and CC 56 the master, scaled from 0..127 to 0..1023. A clock byte
    // inside a message is skipped, and other channels (the device's Drum mode) are ignored.
    port.receive({0xB0, 48, 0xF8, 127, 0xB0, 56, 0, 0x99, 36, 127});
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupFader && e.index == 0 && e.value == gx::kFaderMax);
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupMasterFader && e.value == 0);
    CHECK(!apc.pollEvent(e));

    // The Introduction reply reports all nine faders.
    port.receive({0xF0, 0x47, 0x7F, 0x4F, 0x61, 0x00, 0x09, 0, 127, 64, 0, 0, 0, 0, 0, 100, 0xF7});
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupFader && e.index == 0 && e.value == 0);
    CHECK(apc.pollEvent(e) && e.index == 1 && e.value == gx::kFaderMax);
    CHECK(apc.pollEvent(e) && e.index == 2 && e.value == 64 * 1023 / 127);
    for (int i = 3; i < 8; ++i) CHECK(apc.pollEvent(e) && e.group == gx::kGroupFader);
    CHECK(apc.pollEvent(e) && e.group == gx::kGroupMasterFader && e.value == 100 * 1023 / 127);
    CHECK(!apc.pollEvent(e));

    // LEDs go out paced: each frame sends a limited number of changed pads, taking turns
    // round the grid, so a whole grid takes a few frames rather than one long burst the
    // device would drop. Each pad is a Note On whose channel is the
    // brightness and whose velocity is one of the device's 128 fixed colours. Buttons go as
    // notes on channel 1 every frame.
    port.sent.clear();
    static gx::LedFrame frame;
    frame.clear();
    frame.pads[gx::padIndex(7, 0)] = {255, 128, 1};  // note 0: a bright orange
    frame.right[0] = gx::kWhite;                     // bright: R1 on
    frame.bottom[7] = gx::kFilledColor;              // dim: B8 off
    apc.show(frame);
    CHECK(std::count(port.sent.begin(), port.sent.end(), 0xF0) == 0);  // no SysEx for pads
    CHECK(containsBytes(port.sent, {0x90, 112, 0x01}) && containsBytes(port.sent, {0x90, 107, 0x00}));
    // The bright pad goes out at full brightness, channel 6, with whichever palette entry is
    // nearest its hue. It is the last of the grid in turn order, so watch every frame.
    bool sentNote0 = false;
    uint8_t note0Colour = 0;
    int frames = 1;
    for (;;) {
      for (size_t i = 0; i + 2 < port.sent.size(); i += 3) {
        if (port.sent[i] == 0x96 && port.sent[i + 1] == 0x00) {
          sentNote0 = true;
          note0Colour = port.sent[i + 2];
        }
      }
      if (frames >= 20) break;
      port.sent.clear();
      apc.show(frame);
      if (port.sent.empty()) break;
      CHECK(port.sent.size() % 3 == 0);  // whole messages only
      ++frames;
    }
    CHECK(frames * 24 >= gx::kNumPads && frames > 1);  // paced, not one burst
    CHECK(sentNote0 && note0Colour > 0);

    // With everything sent, one changed pad is three bytes: white at full brightness.
    port.sent.clear();
    frame.pads[gx::padIndex(0, 7)] = gx::kWhite;
    apc.show(frame);
    CHECK(port.sent.size() == 3 && port.sent[0] == 0x96 && port.sent[1] == 0x3F);
    const uint8_t white = port.sent[2];

    CHECK(sameColor(gx::ApcMiniSurface::ledColor(6, white), gx::Rgb{255, 255, 255}));

    // The pads read as barely there below about a fifth of full, so the surface lifts lit
    // colours into a visible range. The UI's dim states — an empty step is 12 of 255 — come
    // out brighter than asked for, in order, and never off.
    const gx::Rgb dimStates[] = {gx::dim(gx::kWhite, 12), gx::kEmptyColor,
                                 gx::dim(gx::kWhite, 40)};
    uint8_t shownBrightness[3] = {0, 0, 0};
    for (size_t i = 0; i < sizeof(dimStates) / sizeof(dimStates[0]); ++i) {
      port.sent.clear();
      frame.pads[gx::padIndex(0, 7)] = dimStates[i];
      apc.show(frame);
      CHECK(port.sent.size() == 3 && (port.sent[0] & 0xF0) == 0x90 && port.sent[2] != 0);
      const gx::Rgb shown = gx::ApcMiniSurface::ledColor(port.sent[0] & 0x0F, port.sent[2]);
      shownBrightness[i] = brightness(shown);
      CHECK(shownBrightness[i] >= 50 && shownBrightness[i] > brightness(dimStates[i]));
    }
    CHECK(shownBrightness[0] <= shownBrightness[1] && shownBrightness[1] < shownBrightness[2]);

    // A dim track colour keeps its hue rather than washing out to white.
    port.sent.clear();
    frame.pads[gx::padIndex(0, 7)] = gx::dim(gx::trackColor(0), 12);  // a faint peach
    apc.show(frame);
    const gx::Rgb faint = gx::ApcMiniSurface::ledColor(port.sent[0] & 0x0F, port.sent[2]);
    // Peach is a pale orange, so "keeps its hue" means the channels stay in the order they
    // are in at full brightness - red, then green, then blue - rather than levelling out
    // into a grey, which is what washing out would look like.
    CHECK(faint.r >= 50 && faint.r > faint.g && faint.g > faint.b);
    // Saturated hues can only be as close as 128 fixed colours allow: the teal of track 5 is
    // the furthest, and even that is near enough to read as the same colour.
    const gx::Rgb hues[] = {gx::kFilledColor, gx::trackColor(0), gx::trackColor(4),
                            gx::kSelectedColor};
    for (size_t i = 0; i < sizeof(hues) / sizeof(hues[0]); ++i) {
      port.sent.clear();
      frame.pads[gx::padIndex(0, 7)] = hues[i];
      apc.show(frame);
      CHECK(port.sent.size() == 3);
      CHECK(colorError(gx::ApcMiniSurface::ledColor(port.sent[0] & 0x0F, port.sent[2]),
                       hues[i]) <= 3 * 56 * 56);
    }

    // A pad that goes black is velocity 0.
    port.sent.clear();
    frame.pads[gx::padIndex(0, 7)] = {0, 0, 0};
    apc.show(frame);
    CHECK(port.sent.size() == 3 && port.sent[2] == 0x00);
    port.sent.clear();
  }
  // Going away turns the LEDs off: every pad to velocity 0, and every button.
  CHECK(containsBytes(port.sent, {0x90, 0x00, 0x00}) && containsBytes(port.sent, {0x90, 0x3F, 0x00}));
  CHECK(std::count(port.sent.begin(), port.sent.end(), 0xF0) == 0);
  CHECK(containsBytes(port.sent, {0x90, 112, 0x00}));

  {
    // Until the device answers the Introduction message, LEDs wait about half a second.
    FakeMidiPort quiet;
    gx::ApcMiniSurface apc(quiet);
    apc.begin();
    quiet.sent.clear();
    static gx::LedFrame dark;
    dark.clear();
    int waited = 0;
    while (quiet.sent.empty() && waited < 100) {
      apc.show(dark);
      ++waited;
    }
    CHECK(waited > 20 && !quiet.sent.empty());
  }
}

void testStepTapAndHold() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;

  // A quick tap (100 ms) switches a step on.
  f.surface.press(gx::kGroupPad, 0);
  f.app.update(1000);
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(1100);
  CHECK(seq.stepActive(0, 0));

  // Holding it (600 ms, past the 500 ms tap limit) shows its note, C2 on the bottom-left
  // key, and leaves it on.
  f.surface.press(gx::kGroupPad, 0);
  f.app.update(2000);
  CHECK(sameColor(frame.pads[0], gx::kSelectedColor));
  CHECK(sameColor(frame.pads[gx::padIndex(7, 0)], gx::kSelectedColor));
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(2600);
  CHECK(seq.stepActive(0, 0));

  // A tap on the lit step doesn't switch it off: only Clear + step removes it.
  f.surface.press(gx::kGroupPad, 0);
  f.app.update(3000);
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(3050);
  CHECK(seq.stepActive(0, 0));
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(3100);
  CHECK(!seq.stepActive(0, 0));

  // Holding an empty step and letting go leaves it empty.
  f.surface.press(gx::kGroupPad, 0);
  f.app.update(4000);
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(4600);
  CHECK(!seq.stepActive(0, 0));

  // In the piano roll, holding a pad plays its note without adding it; a tap adds it.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(gx::padIndex(7, 0));  // piano roll on
  f.surface.tapButton(gx::kButtonNote);
  f.app.update(5000);
  f.surface.press(gx::kGroupPad, gx::padIndex(7, 1));  // C2 at step 2
  f.app.update(6000);
  CHECK(lastNoteOn(f.sink) == 36);
  f.surface.release(gx::kGroupPad, gx::padIndex(7, 1));
  f.app.update(6600);
  CHECK(seq.stepNoteCount(0, 1) == 0);
  f.surface.press(gx::kGroupPad, gx::padIndex(7, 1));
  f.app.update(7000);
  f.surface.release(gx::kGroupPad, gx::padIndex(7, 1));
  f.app.update(7100);
  CHECK(seq.stepActive(0, 1) && seq.stepNote(0, 1) == 36);
}

void testTrackMidiChannel() {
  {
    // Changing a track's channel releases its notes on the old channel, then announces the
    // new channel and sends the preset there.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(seq.trackMidiChannel(0) == 0 && seq.trackMidiChannel(9) == 9);
    seq.setStepNote(0, 0, 36);
    seq.play();
    seq.update(0);
    sink.events.clear();
    seq.setTrackMidiChannel(0, 9);
    CHECK(seq.trackMidiChannel(0) == 9 && sink.events.size() == 3);
    CHECK(sink.is(0, kNoteOff, 0, 36) && sink.is(1, kChannel, 0, 9) && sink.is(2, kPreset, 0, 0));
    seq.setTrackMidiChannel(0, gx::kNumMidiChannels);  // out of range: ignored
    CHECK(seq.trackMidiChannel(0) == 9 && sink.events.size() == 3);
  }

  // Global settings pad 2: rows 3 and 4 are channels 1-16 for the selected track.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kChannel1Pad = gx::padIndex(2, 0);
  const uint8_t kChannel10Pad = gx::padIndex(3, 1);
  tapWithShift(f.surface, gx::kButtonProject);  // Shift + R1
  f.surface.tapPad(1);
  f.app.update(0);
  CHECK(sameColor(frame.pads[1], gx::kSelectedColor));
  CHECK(sameColor(frame.pads[kChannel1Pad], gx::trackColor(0)));  // track 1 is on channel 1
  CHECK(gx::isLit(frame.pads[kChannel10Pad]) && !sameColor(frame.pads[kChannel10Pad], gx::trackColor(0)));

  f.sink.events.clear();
  f.surface.tapPad(kChannel10Pad);
  f.app.update(1);
  CHECK(seq.trackMidiChannel(0) == 9);
  CHECK(f.sink.contains(kChannel, 0, 9) && f.sink.contains(kPreset, 0, 0));
  CHECK(sameColor(frame.pads[kChannel10Pad], gx::trackColor(0)));
  CHECK(!sameColor(frame.pads[kChannel1Pad], gx::trackColor(0)));

  // Another track is one B button away: global settings keeps the mode and the picked
  // setting, so channels can be given out one track after another.
  f.surface.tap(gx::kGroupBottom, 1);
  f.app.update(2);
  CHECK(f.app.ui().mode() == gx::kModeGlobal && f.app.ui().selectedTrack() == 1);
  CHECK(sameColor(frame.pads[1], gx::kSelectedColor));                // still the MIDI setting
  CHECK(sameColor(frame.pads[kChannel1Pad + 1], gx::trackColor(1)));  // track 2 on channel 2
  f.surface.tapPad(kChannel1Pad + 2);
  f.app.update(3);
  CHECK(seq.trackMidiChannel(1) == 2 && seq.trackMidiChannel(0) == 9);
  f.surface.tap(gx::kGroupBottom, 2);
  f.surface.tapPad(kChannel1Pad + 3);
  f.app.update(4);
  CHECK(f.app.ui().mode() == gx::kModeGlobal && seq.trackMidiChannel(2) == 3);

  // Shift + B reaches the other track pages from here, again without leaving.
  if (gx::kNumTracks > gx::kTracksPerPage) {
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 1);
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(5);
    CHECK(f.app.ui().mode() == gx::kModeGlobal);
    CHECK(f.app.ui().selectedTrack() == gx::kTracksPerPage + 2);
    f.surface.tapPad(kChannel1Pad + 4);
    f.app.update(6);
    CHECK(seq.trackMidiChannel(gx::kTracksPerPage + 2) == 4);
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tap(gx::kGroupBottom, 0);
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(7);
  }

  // Tapping B still leaves for note mode from any other mode.
  f.surface.tapButton(gx::kButtonParams);
  f.surface.tap(gx::kGroupBottom, 0);
  f.app.update(8);
  CHECK(f.app.ui().mode() == gx::kModeNote && f.app.ui().selectedTrack() == 0);
  tapWithShift(f.surface, gx::kButtonProject);  // back to global settings for the save check
  f.app.update(9);

  // Channels are saved with the project.
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().trackMidiChannel(0) == 9);
}

void testTrackMidiPort() {
  {
    // Ports default by track page: tracks 1-8 on P1, 9-16 on P2. Changing one releases the
    // track's notes on the old port, then announces the port and sends the preset there.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(seq.trackMidiPort(0) == 0);
    if (gx::kNumTracks > 8) CHECK(seq.trackMidiPort(8) == 1);
    seq.setStepNote(0, 0, 36);
    seq.play();
    seq.update(0);
    sink.events.clear();
    seq.setTrackMidiPort(0, 4);
    CHECK(seq.trackMidiPort(0) == 4 && sink.events.size() == 3);
    CHECK(sink.is(0, kNoteOff, 0, 36) && sink.is(1, kPort, 0, 4) && sink.is(2, kPreset, 0, 0));
    seq.setTrackMidiPort(0, gx::kNumMidiPorts);  // out of range: ignored
    CHECK(seq.trackMidiPort(0) == 4 && sink.events.size() == 3);
  }

  // Global settings pad 2: row 5 holds the ports, below the channels.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kPort1Pad = gx::padIndex(4, 0);
  const uint8_t kPort3Pad = gx::padIndex(4, 2);
  tapWithShift(f.surface, gx::kButtonProject);  // Shift + R1
  f.surface.tapPad(1);                          // the MIDI channel setting
  f.app.update(0);
  CHECK(sameColor(frame.pads[kPort1Pad], gx::trackColor(0)));  // track 1 is on port 1
  CHECK(gx::isLit(frame.pads[kPort3Pad]) && !sameColor(frame.pads[kPort3Pad], gx::trackColor(0)));
  CHECK(std::string(f.app.ui().padLabel(kPort3Pad)) == "P3");

  f.sink.events.clear();
  f.surface.tapPad(kPort3Pad);
  f.app.update(1);
  CHECK(seq.trackMidiPort(0) == 2 && seq.trackMidiChannel(0) == 0);  // the channel is untouched
  CHECK(f.sink.contains(kPort, 0, 2) && f.sink.contains(kPreset, 0, 0));
  CHECK(sameColor(frame.pads[kPort3Pad], gx::trackColor(0)));
  CHECK(!sameColor(frame.pads[kPort1Pad], gx::trackColor(0)));

  // Ports are saved with the project.
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().trackMidiPort(0) == 2);
}

void testTrackInstrument() {
  {
    // The engine: a track plays out of a MIDI port or on an internal instrument, never both.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    CHECK(seq.trackInstrument(0) == gx::kNoInstrument && !seq.trackUsesInstrument(0));
    seq.setStepNote(0, 0, 36);
    seq.play();
    seq.update(0);
    sink.events.clear();

    seq.setTrackInstrument(0, 4);
    CHECK(seq.trackUsesInstrument(0) && seq.trackInstrument(0) == 4);
    CHECK(sink.events.size() == 3);
    CHECK(sink.is(0, kNoteOff, 0, 36) && sink.is(1, kInstrument, 0, 4));
    CHECK(sink.is(2, kPreset, 0, 0));

    // Picking a MIDI port takes it off the instrument.
    sink.events.clear();
    seq.setTrackMidiPort(0, 2);
    CHECK(!seq.trackUsesInstrument(0) && seq.trackMidiPort(0) == 2);
    CHECK(sink.contains(kInstrument, 0, gx::kNoInstrument) && sink.contains(kPort, 0, 2));

    seq.setTrackInstrument(0, gx::kNumInstruments);  // out of range: ignored
    CHECK(!seq.trackUsesInstrument(0));
  }

  // Global settings pad 2: rows 6 and 7 are the instruments, below the ports.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kPort1Pad = gx::padIndex(4, 0);
  const uint8_t kInstrument1Pad = gx::padIndex(5, 0);
  const uint8_t kInstrument12Pad = gx::padIndex(6, 3);
  tapWithShift(f.surface, gx::kButtonProject);  // Shift + R1
  f.surface.tapPad(1);                          // the MIDI channel setting
  f.app.update(0);
  CHECK(std::string(f.app.ui().padLabel(kInstrument1Pad)) == "I1");
  CHECK(std::string(f.app.ui().padLabel(kInstrument12Pad)) == "I12");
  CHECK(sameColor(frame.pads[kPort1Pad], gx::trackColor(0)));  // on its MIDI port to start

  f.sink.events.clear();
  f.surface.tapPad(kInstrument12Pad);
  f.app.update(1);
  CHECK(seq.trackInstrument(0) == 11);
  CHECK(f.sink.contains(kInstrument, 0, 11) && f.sink.contains(kPreset, 0, 0));
  CHECK(sameColor(frame.pads[kInstrument12Pad], gx::trackColor(0)));
  CHECK(!sameColor(frame.pads[kPort1Pad], gx::trackColor(0)));  // the port row dims

  // Tapping the instrument again sends the track back to its MIDI port.
  f.surface.tapPad(kInstrument12Pad);
  f.app.update(2);
  CHECK(!seq.trackUsesInstrument(0) && sameColor(frame.pads[kPort1Pad], gx::trackColor(0)));

  // Instruments are saved with the project.
  f.surface.tapPad(kInstrument1Pad);
  f.app.update(3);
  CHECK(seq.trackInstrument(0) == 0);
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().trackInstrument(0) == 0);
}

size_t countEvents(const RecordingSink& sink, EventType type) {
  size_t count = 0;
  for (size_t i = 0; i < sink.events.size(); ++i) {
    if (sink.events[i].type == type) ++count;
  }
  return count;
}

void testSeparateClock() {
  // For platforms with their own clock: updateSurface() handles input and LEDs without
  // playing anything, and tick() plays on a microsecond clock.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  f.surface.tapPad(0);
  f.surface.tapPad(1);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.updateSurface(0);
  CHECK(f.app.sequencer().playing() && countEvents(f.sink, kNoteOn) == 0);
  f.app.tick(1000);
  CHECK(countEvents(f.sink, kNoteOn) == 1);  // step 1 plays on the first tick
  f.app.tick(125999);  // at 120 BPM step 2 is due exactly 125 ms after step 1
  CHECK(countEvents(f.sink, kNoteOn) == 1);
  f.app.tick(126000);
  CHECK(countEvents(f.sink, kNoteOn) == 2);
  f.app.updateSurface(200);  // the surface can refresh in between without affecting timing
  f.app.tick(250999);
  CHECK(countEvents(f.sink, kNoteOn) == 2);
}

void testShiftButtonLabels() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::UiController& ui = f.app.ui();
  const auto right = [&](uint8_t b) { return std::string(ui.rightButtonLabel(b)); };
  const auto bottom = [&](uint8_t b) {
    const char* label = ui.bottomButtonLabel(b);
    return label ? std::string(label) : std::string("(B)");
  };

  // Without Shift: the button names, and B1..B8.
  CHECK(right(gx::kButtonProject) == "PROJECT" && right(gx::kButtonPlay) == "PLAY");
  CHECK(bottom(0) == "(B)");

  // Shift in note mode: the Shift layer of R1..R8, and track pages on B1..B8.
  f.surface.press(gx::kGroupShift, 0);
  f.app.update(0);
  CHECK(right(gx::kButtonProject) == "GLOBAL" && right(gx::kButtonPattern) == "SCENE");
  CHECK(right(gx::kButtonNote) == "SCALE" && right(gx::kButtonParams) == "PRESET");
  CHECK(right(gx::kButtonClear) == "PROB" && right(gx::kButtonPlay) == "SONG");
  // R6 and R7 are blank: the APC's firmware keeps them for its own Drum and Note modes.
  CHECK(right(gx::kButtonDuplicate) == "" && right(gx::kButtonRecord) == "");
  CHECK(bottom(0) == "T1-8" && bottom(1) == "T9-16");
  CHECK(bottom(gx::kNumTrackPages) == "");  // past the last track page
  f.surface.release(gx::kGroupShift, 0);

  // Pattern mode without Shift: track pages on B1..B4, pattern pages on B5..B8.
  f.surface.tapButton(gx::kButtonPattern);
  f.app.update(1);
  CHECK(bottom(0) == "T1-8" && bottom(kFirstPatternPage) == "PAT 1-8" &&
        bottom(kFirstPatternPage + 1) == "PAT 9-16");
  f.surface.press(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(right(1) == "SCENE" && bottom(1) == "T9-16");  // Shift is the same as anywhere else
  f.surface.release(gx::kGroupShift, 0);

  // Shift on a piano roll track: B5..B8 scroll it.
  f.surface.tapButton(gx::kButtonNote);
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(gx::padIndex(7, 0));  // piano roll on
  f.surface.tapButton(gx::kButtonNote);
  f.surface.press(gx::kGroupShift, 0);
  f.app.update(2);
  CHECK(bottom(4) == "NOTE UP" && bottom(5) == "NOTE DN");
  CHECK(bottom(6) == "STEP <" && bottom(7) == "STEP >" && bottom(0) == "T1-8");
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(3);
  CHECK(bottom(4) == "(B)" && right(gx::kButtonNote) == "NOTE");

  // Holding R3 in note mode: B1..B8 are step pages.
  f.surface.press(gx::kGroupRight, gx::kButtonNote);
  f.app.update(4);
  CHECK(bottom(0) == "S1-32" && bottom(1) == "S33-64" && right(gx::kButtonNote) == "NOTE");
  if (gx::kNumStepPages < gx::kNumBottomButtons) CHECK(bottom(gx::kNumStepPages) == "");
  f.surface.release(gx::kGroupRight, gx::kButtonNote);
  f.app.update(5);
  CHECK(bottom(0) == "(B)");

  // Holding R1: project pages.
  f.surface.press(gx::kGroupRight, gx::kButtonProject);
  f.app.update(6);
  CHECK(bottom(0) == "PG1" && bottom(7) == "PG8");
  f.surface.release(gx::kGroupRight, gx::kButtonProject);
  f.app.update(7);
  CHECK(bottom(7) == "(B)");
}

// Notes the keyboard played since a mark in the sink, in the order they sounded.
uint8_t notesPlayedSince(const Fixture& f, size_t from, uint8_t* out) {
  uint8_t count = 0;
  for (size_t i = from; i < f.sink.events.size() && count < gx::kMaxStepNotes; ++i) {
    if (f.sink.events[i].type == kNoteOn) out[count++] = static_cast<uint8_t>(f.sink.events[i].value);
  }
  return count;
}

void testChordKeys() {
  // The engine: a chord is the scale's own thirds, so whatever the scale, it is in key.
  {
    uint8_t notes[gx::kMaxStepNotes];
    // C major from C3: C E G, and B on top for the seventh.
    CHECK(gx::chordNotes(gx::kScaleMajor, 48, 0, gx::kChordTriad, notes) == 3);
    CHECK(notes[0] == 48 && notes[1] == 52 && notes[2] == 55);
    CHECK(gx::chordNotes(gx::kScaleMajor, 48, 0, gx::kChordSeventh, notes) == 4);
    CHECK(notes[3] == 59);
    // The second degree of the same scale comes out minor, with no chord table anywhere.
    CHECK(gx::chordNotes(gx::kScaleMajor, 48, 1, gx::kChordTriad, notes) == 3);
    CHECK(notes[0] == 50 && notes[1] == 53 && notes[2] == 57);
    // And a minor scale gives a minor chord on its root.
    CHECK(gx::chordNotes(gx::kScaleMinor, 45, 0, gx::kChordTriad, notes) == 3);
    CHECK(notes[0] == 45 && notes[1] == 48 && notes[2] == 52);
    // At the top of the MIDI range it writes what fits.
    CHECK(gx::chordNotes(gx::kScaleMajor, 126, 0, gx::kChordTriad, notes) == 1);
  }

  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kTriadPad = gx::kNumPads - 4;
  const uint8_t kSeventhPad = gx::kNumPads - 3;
  const uint8_t kMajorPad = 4 * gx::kGridCols + gx::kScaleMajor;
  const uint8_t kKey1 = gx::padIndex(7, 0);  // the scale root, C2
  const uint8_t kKey2 = gx::padIndex(7, 1);
  uint8_t notes[gx::kMaxStepNotes];

  // Scale mode: the song is in C major, and both chord pads are lit but dim - chords off.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kMajorPad);
  f.app.update(0);
  const gx::Rgb off = frame.pads[kTriadPad];
  CHECK(gx::isLit(off) && sameColor(off, frame.pads[kSeventhPad]));
  CHECK(seq.trackChord(0) == gx::kChordOff);
  CHECK(std::string(f.app.ui().padLabel(kTriadPad)) == "TRIAD");
  CHECK(std::string(f.app.ui().padLabel(kSeventhPad)) == "7TH");

  // TRIAD lights, and only it: one pad each, so the APC shows which shape is on.
  f.surface.tapPad(kTriadPad);
  f.app.update(1);
  CHECK(seq.trackChord(0) == gx::kChordTriad);
  CHECK(!sameColor(frame.pads[kTriadPad], off) && sameColor(frame.pads[kSeventhPad], off));

  // Note mode: one key now plays three notes of the scale.
  f.surface.tapButton(gx::kButtonNote);
  size_t mark = f.sink.events.size();
  f.surface.tapPad(kKey1);
  f.app.update(2);
  CHECK(notesPlayedSince(f, mark, notes) == 3);
  CHECK(notes[0] == 36 && notes[1] == 40 && notes[2] == 43);  // C2 E2 G2

  // Shift + a key plays the one note, for a line inside a chord track.
  f.surface.press(gx::kGroupShift, 0);
  mark = f.sink.events.size();
  f.surface.tapPad(kKey2);
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(3);
  CHECK(notesPlayedSince(f, mark, notes) == 1 && notes[0] == 38);

  // Held step + key writes the whole chord onto it.
  f.surface.press(gx::kGroupPad, 0);
  f.surface.tapPad(kKey1);
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(4);
  CHECK(seq.stepNoteCount(0, 0) == 3);
  CHECK(seq.stepNote(0, 0, 0) == 36 && seq.stepNote(0, 0, 2) == 43);

  // The same key again takes it out, so one key puts a chord in and lifts it out.
  f.surface.press(gx::kGroupPad, 0);
  f.surface.tapPad(kKey1);
  f.surface.release(gx::kGroupPad, 0);
  f.app.update(5);
  CHECK(seq.stepNoteCount(0, 0) == 0);

  // 7TH replaces TRIAD: four notes, which is exactly a step's chord.
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kSeventhPad);
  f.app.update(6);
  CHECK(seq.trackChord(0) == gx::kChordSeventh);
  CHECK(sameColor(frame.pads[kTriadPad], off));
  f.surface.tapButton(gx::kButtonNote);
  f.surface.press(gx::kGroupPad, 1);
  f.surface.tapPad(kKey1);
  f.surface.release(gx::kGroupPad, 1);
  f.app.update(7);
  CHECK(seq.stepNoteCount(0, 1) == 4 && seq.stepNote(0, 1, 3) == 47);  // C E G B

  // Two chord keys held together: the first fills the step, the second adds what is left,
  // and no note is written twice even where the two chords overlap.
  f.surface.press(gx::kGroupPad, kKey1);
  f.surface.press(gx::kGroupPad, kKey2);
  f.surface.tapPad(2);
  f.surface.release(gx::kGroupPad, kKey2);
  f.surface.release(gx::kGroupPad, kKey1);
  f.app.update(8);
  CHECK(seq.stepNoteCount(0, 2) == gx::kMaxStepNotes);  // a seventh already fills it
  CHECK(seq.stepNote(0, 2, 0) == 36 && seq.stepNote(0, 2, 1) == 40);

  // Chords, drum pads and the piano roll are three ways to use the same grid, so picking one
  // drops the other two: a drum track has no scale to stack on, and the roll has no keyboard.
  const uint8_t kDrumPad = gx::kNumPads - 2;
  const uint8_t kPianoRollPad = gx::padIndex(7, 0);
  const uint8_t kOwnScalePad = gx::kNumPads - 1;
  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kDrumPad);
  f.app.update(9);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardDrums && seq.trackChord(0) == gx::kChordOff);
  CHECK(sameColor(frame.pads[kSeventhPad], off) && sameColor(frame.pads[kTriadPad], off));
  f.surface.tapButton(gx::kButtonNote);  // and the drum pads play one note each
  mark = f.sink.events.size();
  f.surface.tapPad(kKey1);
  f.app.update(10);
  CHECK(notesPlayedSince(f, mark, notes) == 1);

  tapWithShift(f.surface, gx::kButtonNote);
  f.surface.tapPad(kSeventhPad);  // picking a chord takes the track off the drum pads
  f.app.update(11);
  CHECK(seq.trackChord(0) == gx::kChordSeventh);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardProjectScale);

  f.surface.tapPad(kPianoRollPad);
  f.app.update(12);
  CHECK(seq.trackPianoRoll(0) && seq.trackChord(0) == gx::kChordOff);
  f.surface.tapPad(kSeventhPad);
  f.app.update(13);
  CHECK(seq.trackChord(0) == gx::kChordSeventh && !seq.trackPianoRoll(0));

  // An own scale is not in that group: the chord is built from whatever scale the track uses,
  // so the two belong together.
  f.surface.tapPad(kOwnScalePad);
  f.app.update(14);
  CHECK(seq.keyboardLayout(0) == gx::kKeyboardOwnScale);
  CHECK(seq.trackChord(0) == gx::kChordSeventh);

  // Tapping the lit shape turns chords off again.
  f.surface.tapPad(kSeventhPad);
  f.app.update(15);
  CHECK(seq.trackChord(0) == gx::kChordOff);
  f.surface.tapPad(kSeventhPad);
  f.app.update(16);

  // It is saved with the project, and a file from before chords opens with them off.
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().trackChord(0) == gx::kChordSeventh);
  CHECK(reopened->app.sequencer().trackChord(1) == gx::kChordOff);
}

// A platform that has some gear plugged in, for the devices page.
class FakeDevices : public gx::DeviceStatus {
 public:
  FakeDevices() : refreshes(0), surface(false), mixer(false), ports(0), stale(false) {}
  bool deviceConnected(uint8_t device) const override {
    return device == kDeviceSurface ? surface : device == kDeviceMixer ? mixer : false;
  }
  bool portConnected(uint8_t port) const override { return (ports >> port) & 1; }
  const char* outputName() const override { return "MIDI4x4"; }
  bool portsNeedRefresh() const override { return stale; }
  void refreshDevices() override {
    ++refreshes;
    surface = true;          // the APC was plugged in while we looked
    ports |= 0x0F;           // and the four sockets of an interface answered
    stale = false;           // and there is nothing left to offer
  }
  int refreshes;
  bool surface;
  bool mixer;
  uint8_t ports;
  bool stale;
};

// How many events of a type carried this value (a note number, for note on and off).
size_t countNotes(const RecordingSink& sink, EventType type, uint16_t note) {
  size_t count = 0;
  for (size_t i = 0; i < sink.events.size(); ++i) {
    if (sink.events[i].type == type && sink.events[i].value == note) ++count;
  }
  return count;
}

// A keyboard plugged into the sequencer, as the platform hands it over.
class FakeMidiInput : public gx::MidiInput {
 public:
  bool poll(gx::MidiMessage& message) override {
    if (queue.empty()) return false;
    message = queue.front();
    queue.erase(queue.begin());
    return true;
  }
  void bytes(std::initializer_list<uint8_t> stream) {
    gx::MidiParser parser;
    gx::MidiMessage message;
    for (uint8_t byte : stream) {
      if (parser.feed(byte, message)) queue.push_back(message);
    }
  }
  std::vector<gx::MidiMessage> queue;
};

void testMidiParser() {
  gx::MidiParser parser;
  gx::MidiMessage message;
  std::vector<gx::MidiMessage> out;
  // A note on, then another with the status left off: running status, which keyboards use.
  const uint8_t stream[] = {0x90, 60, 100, 62, 80,
                            0xF8,                    // a clock byte in the middle of it
                            64, 90,
                            0x80, 60, 0,             // an explicit note off
                            0xA0, 60, 20,            // aftertouch: understood, not a note
                            0xE0, 0, 64};            // pitch bend: the same
  for (size_t i = 0; i < sizeof(stream); ++i) {
    if (parser.feed(stream[i], message)) out.push_back(message);
  }
  CHECK(out.size() == 7);
  CHECK(out[0].status == 0x90 && out[0].data1 == 60 && out[0].data2 == 100);
  CHECK(out[1].status == 0x90 && out[1].data1 == 62 && out[1].data2 == 80);  // running status
  CHECK(out[2].status == gx::kMidiClock);  // straight through, mid-message
  CHECK(out[3].status == 0x90 && out[3].data1 == 64);  // and the running status survived it
  CHECK(out[4].status == 0x80 && out[4].data1 == 60);
  CHECK(out[5].status == 0xA0 && out[5].data1 == 60);  // passed on for whoever wants it
  CHECK(out[6].status == 0xE0);
}

// Following an outside clock: the source picks the policy, the app decides from moment to
// moment whether a clock is really there, and the engine only obeys.
void testExternalClock() {
  {
    // The engine, driven straight from clocks. A master at 140 while our own project says 120.
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    seq.setBpm(120);
    seq.setTrackLength(0, 4);
    seq.setStepNote(0, 0, 60);
    seq.setStepNote(0, 1, 62);
    CHECK(seq.clockSource() == gx::kClockAuto);  // the default: follow only when there is one
    seq.setFollowingExternal(true);
    seq.play();

    const uint32_t clockUs = 60000000u / (140u * 24u);
    uint32_t t = 0;
    // The first clock after Start is the downbeat, not a step forward.
    seq.externalClockTick(t);
    CHECK(sink.contains(kNoteOn, 0, 60) && !sink.contains(kNoteOn, 0, 62));
    // Six clock intervals make a sixteenth, and the downbeat one didn't advance, so step 2
    // arrives on the seventh clock.
    for (int c = 1; c <= 6; ++c) {
      t += clockUs;
      seq.externalClockTick(t);
    }
    CHECK(sink.contains(kNoteOn, 0, 62));  // step 2, exactly on the master's sixteenth
    CHECK(seq.externalBpm() == 140);       // measured from the gaps, not from the project
    CHECK(seq.bpm() == 120);               // which is left alone

    // Our own clock must not add ticks of its own while someone else is driving.
    const size_t sofar = sink.events.size();
    for (uint32_t i = 0; i < 1000; ++i) seq.updateMicros(t + i * 1000);
    CHECK(sink.events.size() == sofar);  // a second of our time, nothing played
    seq.stop();
  }

  // Through the app, where the policy lives.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  FakeMidiInput master;
  f.app.setMidiInput(&master);
  gx::Sequencer& seq = const_cast<gx::Sequencer&>(f.app.sequencer());

  // Auto, with nothing sending: our own clock, so the box plays rather than looking broken.
  f.app.tick(1000);
  CHECK(!seq.followingExternal());

  // The first clock to arrive is what puts Auto into following, and it has to drive that tick
  // as well: it is the master's downbeat, so step 1 sounds on it rather than a clock later.
  seq.setStepNote(0, 0, 60);
  f.sink.events.clear();
  master.bytes({0xFA});   // start
  master.bytes({0xF8});   // and its first clock
  f.app.tick(2000);
  CHECK(seq.followingExternal() && seq.playing());
  CHECK(f.sink.contains(kNoteOn, 0, 60));

  // When the clock stops, Auto gives up after the timeout and plays on by itself.
  f.app.tick(2000 + 400000);
  CHECK(seq.followingExternal());  // still inside the timeout
  f.app.tick(2000 + 600000);
  CHECK(!seq.followingExternal());

  // On the devices page the three sources sit on row 7, the picked one lit. It goes green
  // while an outside clock is really driving and white while our own is, so AUTO shows which
  // of the two it has settled on without any other display.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapButton(gx::kButtonProject);  // Shift + R1: global settings
  f.surface.release(gx::kGroupShift, 0);
  f.surface.tapPad(3);                      // the DEVICES page
  f.app.updateSurface(3000);
  const uint8_t kClockRow = 6;
  const gx::LedFrame& frame = f.surface.frame;
  // Auto is picked and nothing is arriving, so it reads white: our own clock is driving.
  CHECK(sameColor(frame.pads[gx::padIndex(kClockRow, gx::kClockAuto)], gx::kWhite));
  CHECK(!gx::isLit(frame.pads[gx::padIndex(kClockRow, gx::kClockInternal)]) ||
        !sameColor(frame.pads[gx::padIndex(kClockRow, gx::kClockInternal)], gx::kWhite));

  // A clock arrives: the same pad turns green, which is the whole of the feedback.
  master.bytes({0xF8});
  f.app.tick(700000);
  f.app.updateSurface(3001);
  CHECK(seq.followingExternal());
  CHECK(sameColor(frame.pads[gx::padIndex(kClockRow, gx::kClockAuto)], gx::kSelectedColor));

  // Tapping another picks it.
  f.surface.tapPad(gx::padIndex(kClockRow, gx::kClockInternal));
  f.app.updateSurface(3002);
  CHECK(seq.clockSource() == gx::kClockInternal);

  // On the tempo page while an outside clock is driving, the tempo is not ours to set: the
  // fader and the steppers leave it where it is rather than fighting the master, and the
  // steppers read as unavailable so it doesn't just look broken.
  seq.setClockSource(gx::kClockExternal);
  f.app.tick(720000);
  CHECK(seq.followingExternal());
  f.surface.tapPad(0);  // the TEMPO page
  f.app.updateSurface(3003);
  const uint16_t ourBpm = seq.bpm();
  f.surface.moveFader(gx::kGroupFader, 0, 900);
  f.app.updateSurface(3004);
  CHECK(seq.bpm() == ourBpm);
  const uint8_t kUpPad = gx::padIndex(7, 4);
  f.surface.tapPad(kUpPad);
  f.app.updateSurface(3005);
  CHECK(seq.bpm() == ourBpm);
  CHECK(!sameColor(frame.pads[kUpPad], gx::kWhite));  // not offering a change it won't make

  // Back on our own clock the tempo is ours again.
  seq.setClockSource(gx::kClockInternal);
  f.app.tick(730000);
  f.surface.tapPad(kUpPad);
  f.app.updateSurface(3006);
  CHECK(seq.bpm() == ourBpm + 1);

  // Internal never follows, whatever arrives.
  master.bytes({0xF8});
  f.app.tick(710000);
  CHECK(!seq.followingExternal());

  // External always follows, even before a clock has ever come in - so Play waits for one.
  seq.setClockSource(gx::kClockExternal);
  f.app.tick(800000);
  CHECK(seq.followingExternal());

  // Stop from outside stops us.
  master.bytes({0xFC});
  f.app.tick(900000);
  CHECK(!seq.playing());

  // The clock source is not part of the project: opening another one leaves it where it is,
  // so a project made on someone else's rig can never re-slave this box.
  CHECK(seq.clockSource() == gx::kClockExternal);
  f.app.selectProject(1);
  CHECK(seq.clockSource() == gx::kClockExternal);
  f.app.selectProject(0);
  CHECK(seq.clockSource() == gx::kClockExternal);
}

void testPlayingFromAKeyboard() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  FakeMidiInput keyboard;
  f.app.setMidiInput(&keyboard);
  const gx::Sequencer& seq = f.app.sequencer();

  // A key sounds on the selected track, at the velocity it was played.
  f.sink.events.clear();  // opening a project sent every track's preset and routing
  keyboard.bytes({0x90, 60, 96});
  f.app.update(0);
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kNoteOn, 0, 60));
  CHECK(f.sink.events[0].velocity == 96);
  keyboard.bytes({0x80, 60, 0});
  f.app.update(1);
  CHECK(f.sink.contains(kNoteOff, 0, 60));

  // A note on with no velocity is how many keyboards say note off.
  f.sink.events.clear();
  keyboard.bytes({0x90, 62, 100, 62, 0});
  f.app.update(2);
  CHECK(f.sink.contains(kNoteOn, 0, 62) && f.sink.contains(kNoteOff, 0, 62));

  // It follows the selected track rather than the mode: pick track 3 and play again.
  f.surface.tap(gx::kGroupBottom, 2);
  f.surface.tapButton(gx::kButtonPattern);  // and look at the patterns while playing
  f.sink.events.clear();
  keyboard.bytes({0x90, 64, 70});
  f.app.update(3);
  CHECK(f.app.ui().mode() == gx::kModePattern);
  CHECK(f.sink.events.size() == 1 && f.sink.is(0, kNoteOn, 2, 64));

  // Its transport keys start and stop the sequencer, so a player's hands stay on the keys.
  keyboard.queue.push_back(gx::midiRealtime(gx::kMidiStart));
  f.app.update(4);
  CHECK(seq.playing());
  keyboard.queue.push_back(gx::midiRealtime(gx::kMidiStop));
  f.app.update(5);
  CHECK(!seq.playing());

  // A key held on the keyboard lights the pad that plays the same note, wherever your hands
  // actually are, and goes out when you let go.
  f.surface.tapButton(gx::kButtonNote);
  keyboard.bytes({0x90, 36, 80});  // C2: the bottom-left key of a chromatic keyboard
  f.app.update(4);
  const uint8_t kKey1 = gx::padIndex(7, 0);
  CHECK(sameColor(f.surface.frame.pads[kKey1], gx::kWhite));
  keyboard.bytes({0x80, 36, 0});
  f.app.update(5);
  CHECK(!sameColor(f.surface.frame.pads[kKey1], gx::kWhite));

  // Recording: what is played lands on the step nearest to now, with its velocity.
  f.surface.tapButton(gx::kButtonRecord);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(6);
  CHECK(seq.recording() && seq.playing());
  keyboard.bytes({0x90, 67, 112});
  f.app.update(7);
  const uint16_t step = seq.playhead(2);
  CHECK(seq.stepActive(2, step) && seq.stepHasNote(2, step, 67));
  CHECK(seq.stepVelocity(2, step) == 112);

  // And the playhead turns red while recording, so the grid says what is about to happen.
  const gx::Rgb head = f.surface.frame.pads[step % gx::kStepsPerPage];
  CHECK(head.r > 200 && head.g < 60 && head.b < 60);
  f.surface.tapButton(gx::kButtonRecord);  // armed no more: white again
  f.app.update(8);
  const gx::Rgb white = f.surface.frame.pads[seq.playhead(2) % gx::kStepsPerPage];
  CHECK(white.r > 200 && white.g > 200 && white.b > 200);
}

void testArpeggiator() {
  // The engine: a step's chord plays one note at a time, in the mode's order.
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    const uint8_t chord[] = {60, 64, 67};  // C E G, written low to high
    seq.setStepChord(0, 0, chord, 3);
    seq.setStepGate(0, 0, gx::kGateUnitsPerStep);  // one step long: four notes at 1/64
    CHECK(seq.trackArpMode(0) == gx::kArpOff);

    // Off, the chord sounds together, as it always has.
    seq.play();
    seq.update(0);
    CHECK(countEvents(sink, kNoteOn) == 3);
    seq.stop();

    // Up at 1/64 - six ticks, a quarter of a step - gives C E G C over the step.
    seq.setTrackArpMode(0, gx::kArpUp);
    seq.setTrackArpRate(0, 6);  // kArpRateTicks[6] == 6
    sink.events.clear();
    seq.play();
    seq.update(0);
    CHECK(countNotes(sink, kNoteOn, 60) == 1 && countEvents(sink, kNoteOn) == 1);
    seq.update(40);   // about a third of a step in: two more notes have gone
    CHECK(countEvents(sink, kNoteOn) == 2);
    seq.update(70);
    CHECK(countEvents(sink, kNoteOn) == 3);
    const size_t overOneStep = countEvents(sink, kNoteOn);
    seq.update(124);  // still inside the first step
    CHECK(countEvents(sink, kNoteOn) >= overOneStep);
    CHECK(countNotes(sink, kNoteOn, 64) == 1 && countNotes(sink, kNoteOn, 67) == 1);
    seq.stop();

    // Down starts at the top of the chord.
    seq.setTrackArpMode(0, gx::kArpDown);
    sink.events.clear();
    seq.play();
    seq.update(0);
    CHECK(countNotes(sink, kNoteOn, 67) == 1);
    seq.stop();

    // Two octaves reach an octave above the chord.
    seq.setTrackArpMode(0, gx::kArpUp);
    seq.setTrackArpOctaves(0, 2);
    seq.setStepGate(0, 0, 4 * gx::kGateUnitsPerStep);  // a long run, to hear them all
    sink.events.clear();
    seq.play();
    for (uint32_t ms = 0; ms <= 500; ms += 10) seq.update(ms);
    CHECK(countNotes(sink, kNoteOn, 72) == 1 || countNotes(sink, kNoteOn, 72) > 1);  // C an octave up
    CHECK(countNotes(sink, kNoteOn, 79) >= 1);                                       // and its G
    seq.stop();

    // Out of range settings are refused rather than clamped into nonsense.
    seq.setTrackArpMode(0, 99);
    CHECK(seq.trackArpMode(0) == gx::kArpUp);
    seq.setTrackArpOctaves(0, 0);
    CHECK(seq.trackArpOctaves(0) == 2);
  }

  // The pads: global settings pad 5, in the track's colour, and saved with the project.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kModeUpPad = gx::padIndex(2, gx::kArpUp);
  const uint8_t kRatePad = gx::padIndex(4, 5);
  const uint8_t kOctavePad = gx::padIndex(6, 2);  // three octaves

  tapWithShift(f.surface, gx::kButtonProject);  // Shift + R1
  f.surface.tapPad(gx::kGlobalArp);
  f.app.update(0);
  CHECK(std::string(f.app.ui().padLabel(gx::kGlobalArp)) == "ARP");
  CHECK(std::string(f.app.ui().padLabel(kModeUpPad)) == "UP");
  CHECK(std::string(f.app.ui().padLabel(kRatePad)) == "1/32T");
  CHECK(std::string(f.app.ui().padLabel(kOctavePad)) == "3 OCT");
  CHECK(sameColor(frame.pads[gx::padIndex(2, gx::kArpOff)], gx::trackColor(0)));  // off, for now

  f.surface.tapPad(kModeUpPad);
  f.surface.tapPad(kRatePad);
  f.surface.tapPad(kOctavePad);
  f.app.update(1);
  CHECK(seq.trackArpMode(0) == gx::kArpUp && seq.trackArpRate(0) == 5);
  CHECK(seq.trackArpOctaves(0) == 3);
  CHECK(sameColor(frame.pads[kModeUpPad], gx::trackColor(0)));
  CHECK(sameColor(frame.pads[kRatePad], gx::trackColor(0)));

  // B1..B8 pick another track without leaving the page, so a rig is set up track by track.
  f.surface.tap(gx::kGroupBottom, 1);
  f.app.update(2);
  CHECK(f.app.ui().mode() == gx::kModeGlobal && f.app.ui().selectedTrack() == 1);
  CHECK(seq.trackArpMode(1) == gx::kArpOff);

  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().trackArpMode(0) == gx::kArpUp);
  CHECK(reopened->app.sequencer().trackArpRate(0) == 5);
  CHECK(reopened->app.sequencer().trackArpOctaves(0) == 3);
}

void testRatchets() {
  // The engine: a step that fires more than once spreads its hits evenly across itself.
  {
    RecordingSink sink;
    std::unique_ptr<gx::Sequencer> seqOwner(new gx::Sequencer(sink));
    gx::Sequencer& seq = *seqOwner;
    seq.toggleStep(0, 0);
    seq.setStepNote(0, 0, 60);
    CHECK(seq.stepRatchet(0, 0) == gx::kDefaultRatchet);  // once, as a step always did
    seq.setStepRatchet(0, 0, 4);
    seq.setStepRatchet(0, 1, 99);  // out of range: clamped
    CHECK(seq.stepRatchet(0, 1) == gx::kMaxRatchet);

    // A step is 24 ticks, so four hits land on ticks 0, 6, 12 and 18 of it. At 120 BPM a step
    // is 125 ms, so a quarter of one is about 31 ms.
    seq.play();
    seq.update(0);
    CHECK(countNotes(sink, kNoteOn, 60) == 1);  // the first hit is the step itself
    seq.update(20);
    CHECK(countNotes(sink, kNoteOn, 60) == 1);  // not yet
    seq.update(40);
    CHECK(countNotes(sink, kNoteOn, 60) == 2);
    seq.update(70);
    CHECK(countNotes(sink, kNoteOn, 60) == 3);
    seq.update(100);
    CHECK(countNotes(sink, kNoteOn, 60) == 4);
    seq.update(120);
    CHECK(countNotes(sink, kNoteOn, 60) == 4);  // and no more: the step is done
    // Each hit ends before the next begins.
    CHECK(countNotes(sink, kNoteOff, 60) >= 3);
    seq.stop();
  }

  // The pads: Shift turns the gate lane into the ratchets, and Clear puts it back.
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kGateLanePad = gx::padIndex(6, 3);  // rows 7-8: the gate, or the ratchets

  f.surface.tapPad(0);                      // a step to work on, in note mode
  f.surface.tapButton(gx::kButtonParams);   // R4: velocity and gate
  f.surface.tapPad(0);                      // select the step
  f.app.update(0);
  CHECK(f.app.ui().mode() == gx::kModeStepParams);

  f.surface.press(gx::kGroupShift, 0);
  f.app.update(1);
  CHECK(gx::isLit(frame.pads[gx::padIndex(6, 0)]));              // the first hit, always on
  CHECK(sameColor(frame.pads[gx::padIndex(7, 0)], gx::kBlack));  // past 8 hits: dark
  f.surface.tapPad(kGateLanePad);           // the fourth pad: four hits
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(2);
  CHECK(seq.stepRatchet(0, 0) == 4);
  CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);  // the gate itself is untouched

  // Without Shift the same pads are the gate again - the fourth pad of its ramp, four sixths
  // of a step - and the ratchet behind Shift is left where it was.
  f.surface.tapPad(kGateLanePad);
  f.app.update(3);
  CHECK(seq.stepGate(0, 0) == 4 && seq.stepRatchet(0, 0) == 4);

  // Clear + the step resets everything this mode owns - velocity, gate and the ratchet behind
  // Shift - because Shift + R5 opens another mode and could never be a modifier here.
  f.surface.press(gx::kGroupRight, gx::kButtonClear);
  f.surface.tapPad(0);
  f.surface.release(gx::kGroupRight, gx::kButtonClear);
  f.app.update(4);
  CHECK(seq.stepRatchet(0, 0) == gx::kDefaultRatchet);
  CHECK(seq.stepGate(0, 0) == gx::kDefaultGateUnits);

  // It is saved with the project.
  f.surface.press(gx::kGroupShift, 0);
  f.surface.tapPad(gx::padIndex(6, 7));  // eight hits
  f.surface.release(gx::kGroupShift, 0);
  f.app.update(5);
  CHECK(seq.stepRatchet(0, 0) == 8);
  CHECK(f.app.saveProject());
  std::unique_ptr<Fixture> reopened(new Fixture(storage));
  reopened->app.begin();
  CHECK(reopened->app.sequencer().stepRatchet(0, 0) == 8);
}

void testVelocityFromPads() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::Sequencer& seq = f.app.sequencer();
  const uint8_t kKey1 = gx::padIndex(7, 0);  // the scale root, C2

  // Playing a pad sounds at the velocity it was hit, recording or not.
  f.sink.events.clear();
  f.surface.hitPad(kKey1, 90);
  f.app.update(0);
  bool played = false;
  for (size_t i = 0; i < f.sink.events.size(); ++i) {
    const SinkEvent& e = f.sink.events[i];
    if (e.type == kNoteOn && e.value == 36) played = e.velocity == 90;
  }
  CHECK(played);

  // Recording it puts that velocity on the step, so a part keeps its accents.
  f.surface.tapButton(gx::kButtonRecord);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(1);
  CHECK(seq.recording() && seq.playing());
  f.surface.hitPad(kKey1, 112);
  f.app.update(2);
  const uint16_t step = seq.playhead(0);
  CHECK(seq.stepActive(0, step) && seq.stepNote(0, step) == 36);
  CHECK(seq.stepVelocity(0, step) == 112);

  // A surface that cannot tell how hard - the window's mouse - says nothing, so the note
  // joins the chord and the step keeps the velocity it had.
  f.surface.tapPad(gx::padIndex(7, 1));
  f.app.update(2);  // no time passes: the same step
  CHECK(seq.playhead(0) == step && seq.stepNoteCount(0, step) == 2);
  CHECK(seq.stepVelocity(0, step) == 112);
  f.surface.tapButton(gx::kButtonPlay);
  f.app.update(3);
}

void testDevicesPage() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::LedFrame& frame = f.surface.frame;
  const uint8_t kDevicesPad = gx::kGlobalDevices;
  const uint8_t kPadsPad = gx::padIndex(2, 0);     // row 3: the control surfaces
  const uint8_t kMixerPad = gx::padIndex(2, 1);
  const uint8_t kPort1Pad = gx::padIndex(4, 0);    // row 5: the MIDI ports, as on the routing page
  const uint8_t kPort5Pad = gx::padIndex(4, 4);
  const uint8_t kRefreshPad = gx::padIndex(7, 0);

  // Without a platform to ask, the page shows everything absent and the refresh pad is inert.
  tapWithShift(f.surface, gx::kButtonProject);  // Shift + R1
  f.surface.tapPad(kDevicesPad);
  f.app.update(0);
  CHECK(std::string(f.app.ui().padLabel(kDevicesPad)) == "DEVI-\nCES");
  CHECK(std::string(f.app.ui().padLabel(kPadsPad)) == "PADS");
  CHECK(std::string(f.app.ui().padLabel(kMixerPad)) == "MIXER");
  CHECK(std::string(f.app.ui().padLabel(kPort1Pad)) == "P1");
  CHECK(std::string(f.app.ui().padLabel(kRefreshPad)) == "RE-\nFRESH");
  const gx::Rgb absentSurface = frame.pads[kPadsPad];
  const gx::Rgb absentPort = frame.pads[kPort1Pad];
  CHECK(!sameColor(absentSurface, gx::kWhite) && !sameColor(absentPort, gx::kSelectedColor));
  f.surface.tapPad(kRefreshPad);
  f.app.update(1);

  // With one, lit is here and answering: the mixer alone, and ports P1 and P2.
  FakeDevices devices;
  devices.mixer = true;
  devices.ports = 0x03;
  f.app.ui().setDeviceStatus(&devices);
  f.app.update(2);
  CHECK(sameColor(frame.pads[kMixerPad], gx::kWhite));
  CHECK(sameColor(frame.pads[kPadsPad], absentSurface));
  CHECK(sameColor(frame.pads[kPort1Pad], gx::kSelectedColor));
  CHECK(sameColor(frame.pads[kPort5Pad], absentPort));

  // The refresh pad goes and looks again, and what it finds shows up.
  f.surface.tapPad(kRefreshPad);
  f.app.update(3);
  CHECK(devices.refreshes == 1);
  CHECK(sameColor(frame.pads[kPadsPad], gx::kWhite));      // the APC turned up
  CHECK(sameColor(frame.pads[kPort5Pad], absentPort));     // a 4x4 leaves P5-P8 empty
  CHECK(sameColor(frame.pads[gx::padIndex(4, 3)], gx::kSelectedColor));

  // The refresh pad brightens when gear has come or gone that the ports aren't wired to:
  // it offers the rewire rather than doing it, because a port changing under a take is the
  // player's call. Pressing it takes the offer away.
  const gx::Rgb idleRefresh = frame.pads[kRefreshPad];
  devices.stale = true;
  f.app.update(3);
  const gx::Rgb waitingRefresh = frame.pads[kRefreshPad];
  CHECK(brightness(waitingRefresh) > brightness(idleRefresh));
  f.surface.tapPad(kRefreshPad);
  f.app.update(3);
  CHECK(devices.refreshes == 2 && !devices.stale);
  CHECK(sameColor(frame.pads[kRefreshPad], idleRefresh));

  // The other settings still work, and nothing else on the page is a refresh.
  f.surface.tapPad(gx::kGlobalTempo);
  f.surface.tapPad(kRefreshPad);
  f.app.update(4);
  CHECK(devices.refreshes == 2);
  CHECK(f.app.sequencer().bpm() == 120);
}

void testKeyboardOctave() {
  MemoryStorage storage;
  {
    std::unique_ptr<Fixture> fOwner(new Fixture(storage));
    Fixture& f = *fOwner;
    f.app.begin();
    const gx::Sequencer& seq = f.app.sequencer();
    CHECK(seq.keyboardOctave(0) == gx::kDefaultKeyboardOctave);

    // Hold step 1 and tap the bottom-left key: C2 (36) in the default C chromatic.
    f.surface.press(gx::kGroupPad, 0);
    f.surface.tapPad(gx::padIndex(7, 0));
    f.surface.release(gx::kGroupPad, 0);
    f.app.update(0);
    CHECK(seq.stepNote(0, 0) == 36);

    // Shift + the leftmost key drops an octave, and plays nothing.
    const size_t playedBefore = f.sink.events.size();
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tapPad(gx::padIndex(7, 0));
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(1);
    CHECK(seq.keyboardOctave(0) == 1);
    CHECK(f.sink.events.size() == playedBefore);

    f.surface.press(gx::kGroupPad, 1);
    f.surface.tapPad(gx::padIndex(7, 0));
    f.surface.release(gx::kGroupPad, 1);
    f.app.update(2);
    CHECK(seq.stepNote(0, 1) == 24);  // the same pad is now C1

    // Shift + the rightmost key raises it; keys in between do nothing.
    f.surface.press(gx::kGroupShift, 0);
    f.surface.tapPad(gx::padIndex(6, 7));
    f.surface.tapPad(gx::padIndex(6, 7));
    f.surface.tapPad(gx::padIndex(7, 3));
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(3);
    CHECK(seq.keyboardOctave(0) == 3);

    // It stops at the bottom of the range instead of wrapping.
    f.surface.press(gx::kGroupShift, 0);
    for (int i = 0; i < 6; ++i) f.surface.tapPad(gx::padIndex(7, 0));
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(4);
    CHECK(seq.keyboardOctave(0) == 0);

    // The octave belongs to the track.
    f.surface.tap(gx::kGroupBottom, 1);
    f.app.update(5);
    CHECK(seq.keyboardOctave(1) == gx::kDefaultKeyboardOctave && seq.keyboardOctave(0) == 0);
    CHECK(f.app.saveProject());
  }

  std::unique_ptr<Fixture> reopenedOwner(new Fixture(storage));
  Fixture& reopened = *reopenedOwner;
  reopened.app.begin();  // the octave is saved with the project
  CHECK(reopened.app.sequencer().keyboardOctave(0) == 0);
}

void testScales() {
  CHECK(std::strcmp(gx::scaleName(gx::kScaleMajor), "MAJOR") == 0);
  CHECK(std::strcmp(gx::rootName(9), "A") == 0);
  // C major from C2: C D E F G A B, then C3.
  CHECK(gx::scaleNote(gx::kScaleMajor, 36, 0) == 36 && gx::scaleNote(gx::kScaleMajor, 36, 2) == 40);
  CHECK(gx::scaleNote(gx::kScaleMajor, 36, 7) == 48);
  CHECK(gx::scaleDegree(gx::kScaleMajor, 36, 41) == 3);               // F
  CHECK(gx::scaleDegree(gx::kScaleMajor, 36, 37) == gx::kNoDegree);   // C# isn't in C major
  CHECK(gx::scaleNote(gx::kScaleMinorPentatonic, 45, 3) == 52);        // A C D E: E3
  CHECK(gx::scaleNote(gx::kScaleChromatic, 120, 8) == gx::kInvalidNote);  // above MIDI 127

  // Every table rises within one octave, and notes and degrees convert both ways.
  for (uint8_t s = 0; s < gx::kNumScales; ++s) {
    const uint8_t length = gx::scaleLength(s);
    CHECK(length >= 5 && length <= 12);
    CHECK(gx::scaleNote(s, 36, 0) == 36 && gx::scaleNote(s, 36, length) == 48);
    uint8_t previous = 0;
    for (uint8_t degree = 0; degree < 40; ++degree) {
      const uint8_t note = gx::scaleNote(s, 36, degree);
      if (note == gx::kInvalidNote) break;
      CHECK(degree == 0 || note > previous);
      CHECK(gx::scaleDegree(s, 36, note) == degree);
      previous = note;
    }
  }
}

void testScaleMode() {
  const uint8_t kFirstScalePad = 32;  // the bottom four rows
  MemoryStorage storage;
  {
    std::unique_ptr<Fixture> fOwner(new Fixture(storage));
    Fixture& f = *fOwner;
    f.app.begin();
    const gx::Sequencer& seq = f.app.sequencer();
    const gx::LedFrame& frame = f.surface.frame;

    f.surface.press(gx::kGroupShift, 0);
    f.surface.tapButton(gx::kButtonNote);  // Shift + R3
    f.surface.release(gx::kGroupShift, 0);
    f.app.update(0);
    CHECK(f.app.ui().mode() == gx::kModeScale);
    CHECK(sameColor(frame.pads[gx::padIndex(1, 0)], gx::kSelectedColor));  // root C, both C keys
    CHECK(sameColor(frame.pads[gx::padIndex(1, 7)], gx::kSelectedColor));
    CHECK(gx::isLit(frame.pads[gx::padIndex(0, 1)]));                   // C# glows lightly
    CHECK(!sameColor(frame.pads[gx::padIndex(0, 1)], gx::kSelectedColor));
    CHECK(sameColor(frame.pads[gx::padIndex(0, 0)], gx::kBlack));       // no black key there
    CHECK(sameColor(frame.pads[kFirstScalePad + gx::kScaleChromatic], gx::kSelectedColor));
    CHECK(sameColor(frame.pads[kFirstScalePad + gx::kScaleMajor], gx::kFilledColor));
    CHECK(sameColor(frame.right[gx::kButtonNote], gx::kWhite));  // R3 leads back to note mode

    f.surface.tapPad(gx::padIndex(1, 5));  // A
    f.surface.tapPad(kFirstScalePad + gx::kScaleMinor);
    f.app.update(1);
    CHECK(seq.scaleRoot() == 9 && seq.scale() == gx::kScaleMinor);
    CHECK(sameColor(frame.pads[gx::padIndex(1, 5)], gx::kSelectedColor));
    CHECK(!sameColor(frame.pads[gx::padIndex(1, 0)], gx::kSelectedColor));

    // R3 returns to note mode, whose keyboard is now A minor from A2: A2 B2 C3 ... G3 A3 B3.
    f.surface.tapButton(gx::kButtonNote);
    f.surface.press(gx::kGroupPad, 0);
    f.surface.tapPad(gx::padIndex(7, 1));  // second key: B2
    f.surface.release(gx::kGroupPad, 0);
    // A 7-note scale fills a row with one octave: the eighth pad of a row and the first pad
    // of the row above are both A3.
    f.surface.press(gx::kGroupPad, 1);
    f.surface.tapPad(gx::padIndex(7, 7));
    f.surface.release(gx::kGroupPad, 1);
    f.surface.press(gx::kGroupPad, 2);
    f.surface.tapPad(gx::padIndex(6, 0));
    f.surface.release(gx::kGroupPad, 2);
    f.surface.press(gx::kGroupPad, 0);  // hold step 1 to light its note
    f.app.update(2);
    CHECK(f.app.ui().mode() == gx::kModeNote);
    CHECK(seq.stepNote(0, 0) == 47);
    CHECK(seq.stepNote(0, 1) == 57 && seq.stepNote(0, 2) == 57);
    CHECK(sameColor(frame.pads[gx::padIndex(7, 1)], gx::kSelectedColor));
    CHECK(!sameColor(frame.pads[gx::padIndex(7, 0)], gx::kEmptyColor));  // root A is tinted
    CHECK(f.app.saveProject());
  }

  std::unique_ptr<Fixture> reopenedOwner(new Fixture(storage));
  Fixture& reopened = *reopenedOwner;  // the scale is saved with the project
  reopened.app.begin();
  const gx::Sequencer& seq = reopened.app.sequencer();
  CHECK(seq.scaleRoot() == 9 && seq.scale() == gx::kScaleMinor);

  // Shift + R3 opens scale mode from pattern mode too.
  reopened.surface.tapButton(gx::kButtonPattern);
  reopened.surface.press(gx::kGroupShift, 0);
  reopened.surface.tapButton(gx::kButtonNote);
  reopened.surface.release(gx::kGroupShift, 0);
  reopened.app.update(0);
  CHECK(reopened.app.ui().mode() == gx::kModeScale);
}

}  // namespace

// Every pad label in the current mode fits: kPadLabelLines lines of kPadLabelLineChars.
void checkPadLabelsFit(const gx::UiController& ui) {
  for (uint8_t pad = 0; pad < gx::kNumPads; ++pad) {
    const char* text = ui.padLabel(pad);
    if (!text) continue;
    int lines = 1, length = 0, longest = 0;
    for (const char* c = text; *c; ++c) {
      if (*c == '\n') {
        ++lines;
        length = 0;
      } else if (++length > longest) {
        longest = length;
      }
    }
    CHECK(lines <= gx::kPadLabelLines && longest <= gx::kPadLabelLineChars);
  }
}

void testPadLabels() {
  MemoryStorage storage;
  std::unique_ptr<Fixture> fOwner(new Fixture(storage));
  Fixture& f = *fOwner;
  f.app.begin();
  const gx::UiController& ui = f.app.ui();
  const auto label = [&](uint8_t pad) {
    const char* text = ui.padLabel(pad);
    return text ? std::string(text) : std::string("(none)");
  };
  CHECK(label(0) == "(none)");  // note mode has none

  // Scale mode: note names, scale names and the track's keyboard pads.
  tapWithShift(f.surface, gx::kButtonNote);
  f.app.update(0);
  CHECK(ui.mode() == gx::kModeScale);
  CHECK(label(gx::padIndex(1, 0)) == "C" && label(gx::padIndex(0, 1)) == "C#");
  CHECK(label(gx::padIndex(0, 0)) == "(none)");  // no black key left of C
  CHECK(label(32) == "MAJOR" && label(32 + 5) == "MIXO-\nLYDIAN");
  CHECK(label(gx::padIndex(7, 0)) == "PIANO\nROLL" && label(gx::kNumPads - 2) == "DRUMS");
  CHECK(label(gx::kNumPads - 4) == "TRIAD" && label(gx::kNumPads - 3) == "7TH");
  CHECK(label(gx::padIndex(7, 1)) == "(none)");  // the gap between the roll and the keys
  CHECK(label(gx::kNumPads - 1) == "OWN\nSCALE");
  checkPadLabelsFit(ui);

  // Global settings: the settings, then the picked setting's pads.
  tapWithShift(f.surface, gx::kButtonProject);
  f.app.update(1);
  CHECK(ui.mode() == gx::kModeGlobal);
  CHECK(label(0) == "TEMPO" && label(1) == "MIDI\nCH" && label(16) == "(none)");
  f.surface.tapPad(1);
  f.app.update(2);
  CHECK(label(16) == "1" && label(31) == "16");
  checkPadLabelsFit(ui);
  f.surface.tapPad(0);
  f.app.update(3);
  CHECK(label(gx::padIndex(7, 4)) == "+1" && label(gx::padIndex(7, 5)) == "-1");
  CHECK(label(16) == "(none)");
  checkPadLabelsFit(ui);
}

int main(int argc, char* argv[]) {
  testSequencerPlaysNotesOnTime();
  testTempoDoesNotDrift();
  testTracksWrapAtTheirOwnLength();
  testPatterns();
  testStepEditing();
  testRecordQuantisesToNearestStep();
  testPresets();
  testProjectCodec();
  testProjectExtras();
  testSlotStore();
  testFileStorage(argc > 1 ? argv[1] : ".");
  testMidiEventSink();
  testMidiClock();
#ifdef GX_HAVE_AUDIO
  testSpscQueue();
  testAudioEngine();
  testInstrumentConfig();
  testInstrumentPanning();
  testWavWriter();
  testInstrumentAudioPath();
#endif
  testTransportButtons();
  testTrackButtonsReturnToNoteMode();
  testShiftLightsWhileHeld();
  testNoteMode();
  testProjectMode();
  testProjectPages();
  testPatternMode();
  testPatternPages();
  testTrackPagesInNoteMode();
  testTrackPageLimit();
  testStepPagesAndLength();
  testChordPlayback();
  testStepProbability();
  testChordEditing();
  testStepParamsMode();
  testStepGate();
  testProbabilityMode();
  testFaders();
  testControlMap();
  testControlCc();
  testMixer();
  testScenes();
  testScenePatterns();
  testSendAllControls();
  testSwing();
  testMicroTiming();
  testBarClock();
  testSongPlayback();
  testArrangement();
  testMuteSolo();
  testGlobalMode();
  testKeyboardLayouts();
  testChordKeys();
  testMidiParser();
  testPlayingFromAKeyboard();
  testExternalClock();
  testArpeggiator();
  testRatchets();
  testVelocityFromPads();
  testDevicesPage();
  testPianoRoll();
  testStepTapAndHold();
  testTrackMidiChannel();
  testTrackMidiPort();
  testTrackInstrument();
  testSeparateClock();
  testShiftButtonLabels();
  testApcMiniSurface();
  testKeyboardOctave();
  testScales();
  testScaleMode();
  testPadLabels();
  testPresetMode();
  testPresetPages();
  testAppKeepsDefaultsWhenStorageIsCorrupt();
  testOldSessionFormatStillOpens();

  if (gFailures) {
    std::printf("%d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
