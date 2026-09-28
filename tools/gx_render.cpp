// Renders the tracks routed to internal instruments into a WAV file, with no audio device.
//
//   gx_render --out FILE.wav [--data DIR] [--instruments FILE] [--demo] [--seconds S]
//             [--rate HZ] [--block FRAMES]
//
//   --data DIR    projects to open (default: none, which starts on an empty project)
//   --instruments what each instrument slot hosts, and its level and pan
//   --demo        program a short pattern on track 1 and route it to instrument I1
//   --seconds S   how much to render (default 4)
//   --rate HZ     sample rate (default 48000)
//   --block N     frames per block, as a device would deliver them (default 128)
//
// It also reports how long the render took against how much audio came out, which is the
// measurement that sizes a board: "12.4x realtime" means about 8% of one core.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>

#include "app/GroovixApp.h"
#include "audio/AudioEngine.h"
#include "audio/InstrumentConfig.h"
#include "audio/InstrumentRack.h"
#include "audio/InstrumentState.h"
#include "audio/NullBackend.h"
#include "audio/WavWriter.h"
#include "comm/MidiEventSink.h"
#include "common/FileStorage.h"
#include "common/LogMidiOutput.h"

namespace {

// Presses the surface the way a player would, so the tool sets things up through the real UI.
class ScriptedSurface : public gx::ControlSurface {
 public:
  bool pollEvent(gx::ControlEvent& event) override {
    if (queue_.empty()) return false;
    event = queue_.front();
    queue_.pop_front();
    return true;
  }
  void show(const gx::LedFrame&) override {}

  void tap(uint8_t group, uint8_t index) {
    const gx::ControlEvent down = {group, index, true, 0, false, 0};
    const gx::ControlEvent up = {group, index, false, 0, false, 0};
    queue_.push_back(down);
    queue_.push_back(up);
  }
  void tapPad(uint8_t pad) { tap(gx::kGroupPad, pad); }
  void tapWithShift(uint8_t button) {
    const gx::ControlEvent shiftDown = {gx::kGroupShift, 0, true, 0, false, 0};
    const gx::ControlEvent shiftUp = {gx::kGroupShift, 0, false, 0, false, 0};
    queue_.push_back(shiftDown);
    tap(gx::kGroupRight, button);
    queue_.push_back(shiftUp);
  }

 private:
  std::deque<gx::ControlEvent> queue_;
};

double nowSeconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* outPath = NULL;
  const char* dataDir = "";
  const char* instrumentsPath = NULL;
  bool demo = false;
  double seconds = 4.0;
  uint32_t rate = 48000;
  uint32_t block = 128;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      outPath = argv[++i];
    } else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
      dataDir = argv[++i];
    } else if (std::strcmp(argv[i], "--instruments") == 0 && i + 1 < argc) {
      instrumentsPath = argv[++i];
    } else if (std::strcmp(argv[i], "--demo") == 0) {
      demo = true;
    } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
      rate = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
      block = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr,
                   "usage: %s --out FILE.wav [--data DIR] [--demo] [--seconds S] [--rate HZ] "
                   "[--block FRAMES]\n",
                   argv[0]);
      return 2;
    }
  }
  if (!outPath) {
    std::fprintf(stderr, "%s: --out is required\n", argv[0]);
    return 2;
  }
  // A zero block would never advance the render loop, and a zero rate makes a file no
  // player will open. Say so instead of spinning or writing nonsense.
  if (block < 1 || block > 8192) {
    std::fprintf(stderr, "%s: --block must be 1 to 8192 frames\n", argv[0]);
    return 2;
  }
  if (rate < 8000 || rate > 192000) {
    std::fprintf(stderr, "%s: --rate must be 8000 to 192000 Hz\n", argv[0]);
    return 2;
  }
  if (seconds <= 0.0) {
    std::fprintf(stderr, "%s: --seconds must be more than 0\n", argv[0]);
    return 2;
  }

  ScriptedSurface surface;
  gx::FileStorage storage(dataDir);
  gx::LogMidiOutput midiOut(NULL);  // tracks on MIDI ports are not part of the render
  gx::MidiEventSink sink(midiOut);
  gx::AudioEngine engine;
  sink.setInstruments(&engine);

  gx::InstrumentConfig config;
  if (instrumentsPath) {
    uint16_t errorLine = 0;
    if (!config.loadFile(instrumentsPath, &errorLine)) {
      std::fprintf(stderr, "%s: %s%s", argv[0], instrumentsPath,
                   errorLine ? " has a bad line\n" : " cannot be opened\n");
      return 2;
    }
    config.applyTo(engine);
    std::printf("instruments: %s, %u slots name a plugin\n", instrumentsPath,
                config.assignedSlots());
  }

  std::unique_ptr<gx::GroovixApp> app(new gx::GroovixApp(surface, storage, sink));
  app->begin();

  if (demo) {
    // Four steps on track 1, then Global settings -> MIDI channel -> instrument I1.
    for (uint8_t step = 0; step < 16; step += 4) surface.tapPad(step);
    surface.tapWithShift(gx::kButtonProject);
    surface.tapPad(1);   // the MIDI channel setting
    surface.tapPad(40);  // row 6, first pad: instrument I1
    surface.tap(gx::kGroupRight, gx::kButtonNote);
    app->updateSurface(0);  // handles the presses without starting the sequencer's clock
  }
  surface.tap(gx::kGroupRight, gx::kButtonPlay);
  app->updateSurface(0);

  gx::NullBackend backend(rate, block);
  if (!backend.start(engine)) return 1;
  gx::InstrumentRack rack;
  gx::InstrumentState instrumentState(storage, rack);
  rack.load(config, engine, rate, block);
  instrumentState.openProject(app->currentProject());  // the sounds this project was saved with
  if (instrumentState.slotsRestored() > 0) {
    std::printf("instruments: %u slot(s) restored from the project\n",
                instrumentState.slotsRestored());
  }
  gx::WavWriter wav;
  if (!wav.open(outPath, rate)) {
    std::fprintf(stderr, "%s: cannot write %s\n", argv[0], outPath);
    return 1;
  }

  const uint32_t totalFrames = static_cast<uint32_t>(seconds * rate);
  const double blockMicros = 1e6 * block / rate;
  double micros = 0;
  const double started = nowSeconds();
  for (uint32_t frames = 0; frames < totalFrames; frames += block) {
    app->tick(static_cast<uint32_t>(micros));  // the sequencer runs a block at a time
    backend.renderBlock();
    wav.write(backend.left(), backend.right(), block);
    micros += blockMicros;
  }
  const double took = nowSeconds() - started;

  rack.unload(engine);
  if (!wav.close()) {
    std::fprintf(stderr, "%s: writing %s failed\n", argv[0], outPath);
    return 1;
  }
  std::printf("%s: %.1f s of audio at %u Hz, %u-frame blocks\n", outPath,
              wav.framesWritten() / static_cast<double>(rate), rate, block);
  if (took > 0.0) {
    std::printf("rendered in %.2f s — %.1fx realtime (about %.0f%% of one core)\n", took,
                seconds / took, 100.0 * took / seconds);
  } else {
    std::printf("rendered in under a millisecond — too fast to measure\n");
  }
  std::printf("voices at the end: %u, events dropped: %u\n", engine.voicesPlaying(),
              engine.droppedEvents());
  return 0;
}
