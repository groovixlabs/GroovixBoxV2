// SDL2 simulator: runs the portable GroovixApp with desktop implementations of its
// platform interfaces.
//
//   groovix_sim [--data DIR] [--config DIR] [--controls FILE] [--midi-log] [--no-apc]
//               [--audio none|jack|alsa] [--audio-device NAME] [--rate HZ] [--block FRAMES]
//               [--screenshot FILE.bmp]
//
//   --data DIR         where projects and presets are stored
//                      (default: $GXBOX_DATA_DIR, else /mnt/usb1/data)
//   --config DIR       where controls.conf and instruments.conf are read from, so the rig's
//                      settings can live apart from its projects - beside your dotfiles, or
//                      on read-only media (default: $GXBOX_CONFIG_DIR, else /mnt/usb1/config)
//   --controls FILE    which MIDI CC each fader and knob sends, named outright
//                      (default: controls.conf in the config directory, if it is there)
//   --instruments FILE what each internal instrument slot hosts, and its level and pan
//                      (default: instruments.conf in the config directory, if it is there)
//   --audio BACKEND    play the tracks routed to internal instruments I1-I16:
//                      jack (or PipeWire answering it), alsa, or none (the default)
//   --audio-device N   ALSA device, e.g. hw:0 or default
//   --rate HZ          sample rate to ask the device for (default 48000)
//   --block FRAMES     frames per block: smaller is tighter but more fragile (default 128)
//   --midi-log         print the MIDI messages the sequencer sends
//   --no-midi-out      don't wire P1..P8 to anything; patch them by hand instead
//   --no-apc           don't use a connected Akai APC mini mk2 as the control surface
//   --screenshot FILE  render a few frames to a BMP and exit without saving
//
// A connected APC mini mk2 is used as the control surface, with the window mirroring it.

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <strings.h>
#include <memory>
#include <mutex>
#include <string>

#include "app/GroovixApp.h"
#include "linux/MidiRig.h"
#include "linux/RigConfig.h"
#include "linux/SingleInstance.h"
#include "audio/AudioEngine.h"
#include "audio/InstrumentConfig.h"
#include "audio/InstrumentRack.h"
#include "audio/InstrumentState.h"
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "common/ApcMiniSurface.h"
#include "common/MidiMixSurface.h"
#include "common/ClockThread.h"
#include "comm/MidiEventSink.h"
#include "common/FileStorage.h"
#include "common/LogMidiOutput.h"
#include "common/RigPaths.h"
#include "sdl/SdlSurface.h"
#ifdef GX_AUDIO_JACK
#include "audio/JackBackend.h"
#endif
#ifdef GX_AUDIO_ALSA
#include "audio/AlsaBackend.h"
#endif
#ifdef GX_HAVE_ALSA
#include "linux/AlsaMidiOutput.h"
#include "linux/AlsaMidiPort.h"
#endif

namespace {

const int kScreenshotFrames = 5;
const char* const kLayoutTags[gx::kNumKeyboardLayouts] = {"", " OWN", " DRM"};

// Sends every message to a real port and to the log, for --midi-log.
class TeeMidiOutput : public gx::MidiOutput {
 public:
  TeeMidiOutput(gx::MidiOutput& first, gx::MidiOutput& second) : first_(first), second_(second) {}
  void send(const gx::MidiMessage& message) override {
    first_.send(message);
    second_.send(message);
  }

 private:
  gx::MidiOutput& first_;
  gx::MidiOutput& second_;
};

// Defaults live with the members rather than in a braced list at the call site: a list has to
// be counted against the struct every time either changes, and it silently means something
// else when it isn't.
struct Options {
  bool allowMultiple = false;  // for headless screenshots beside a running simulator
  const char* screenshotPath = NULL;
  const char* midiOutDevice = NULL;  // ALSA client to connect P1.. to, e.g. "MIDI4x4"
  bool noMidiOut = false;            // leave the ports unwired: patch them by hand instead
  const char* dataDir = NULL;
  const char* configDir = NULL;
  const char* controlsPath = NULL;
  const char* instrumentsPath = NULL;
  const char* audioBackend = "none";
  const char* audioDevice = "default";
  uint32_t sampleRate = 48000;
  uint32_t blockFrames = 128;
  bool midiLog = false;
  bool useApc = true;
};

// Opens the audio device the options ask for. Returns nothing for "none", or when the
// backend isn't built in or won't open — the sequencer still runs, MIDI still plays, and the
// internal instruments stay silent.
std::unique_ptr<gx::AudioBackend> openAudio(const Options& options, gx::AudioEngine& engine) {
  const std::string backend = options.audioBackend ? options.audioBackend : "none";
  std::unique_ptr<gx::AudioBackend> audio;
  if (backend == "none") return audio;

  if (backend == "jack") {
#ifdef GX_AUDIO_JACK
    audio.reset(new gx::JackBackend("GroovixBox"));
#else
    std::fprintf(stderr, "audio: this build has no JACK backend\n");
#endif
  } else if (backend == "alsa") {
#ifdef GX_AUDIO_ALSA
    audio.reset(new gx::AlsaBackend(options.audioDevice ? options.audioDevice : "default",
                                    options.sampleRate, options.blockFrames));
#else
    std::fprintf(stderr, "audio: this build has no ALSA backend\n");
#endif
  } else {
    std::fprintf(stderr, "audio: unknown backend '%s'\n", backend.c_str());
  }

  if (audio && !audio->start(engine)) audio.reset();
  if (audio) {
    std::printf("audio: %s, %u Hz, %u-frame blocks (%.1f ms)\n", audio->name(),
                audio->sampleRate(), audio->blockSize(),
                1000.0 * audio->blockSize() / audio->sampleRate());
#ifdef GX_AUDIO_ALSA
    const gx::AlsaBackend* alsa = dynamic_cast<const gx::AlsaBackend*>(audio.get());
    if (alsa && !alsa->realTime()) {
      std::printf("audio: no real-time priority (see rtprio in /etc/security/limits.d)\n");
    }
#endif
  } else if (backend != "none") {
    std::fprintf(stderr, "audio: carrying on without it; instruments I1-I16 stay silent\n");
  }
  return audio;
}

void loadInstruments(gx::AudioEngine& engine, gx::InstrumentConfig& config,
                     const std::string& path, bool required) {
  uint16_t errorLine = 0;
  if (!config.loadFile(path, &errorLine)) {
    if (errorLine > 0) {
      std::fprintf(stderr, "instruments: %s line %u is not valid; the rest was applied\n",
                   path.c_str(), errorLine);
    } else if (required) {
      std::fprintf(stderr, "instruments: cannot open %s\n", path.c_str());
      return;
    } else {
      return;  // no file at the default path: nothing to say
    }
  }
  config.applyTo(engine);
  std::printf("instruments: %s — %u of %u slots name a plugin\n", path.c_str(),
              config.assignedSlots(), gx::kNumInstruments);
  if (config.assignedSlots() > 0 && !gx::InstrumentRack::supported()) {
    std::fprintf(stderr, "instruments: this build cannot host plugins (built without lilv)\n");
  }
}

// The simulator window and, when one is connected, a hardware surface as one: events come
// from both, and the LEDs show on both. Hardware faders also move the window's faders.
// The window and the hardware together: a press on either reaches the app, and the LEDs go to
// both. A fader moved on the panel moves on screen too, so the window always shows what the
// hardware is doing.
class MirroredSurface : public gx::ControlSurface {
 public:
  MirroredSurface(gx::SdlSurface& window, gx::ControlSurface* hardware)
      : window_(window), hardware_(hardware) {}

  bool pollEvent(gx::ControlEvent& event) override {
    if (hardware_ && hardware_->pollEvent(event)) {
      if (event.group == gx::kGroupFader || event.group == gx::kGroupMasterFader ||
          event.group == gx::kGroupMixFader || event.group == gx::kGroupMixMaster ||
          event.group == gx::kGroupKnob) {
        window_.setFaderPosition(event.group, event.index, event.value);
      }
      if (std::getenv("GX_DEBUG_INPUT")) {
        std::printf("input: group %d index %d pressed %d value %d\n", event.group, event.index,
                    event.pressed, event.value);
      }
      return true;
    }
    return window_.pollEvent(event);
  }

  void show(const gx::LedFrame& frame) override {
    window_.show(frame);
    if (hardware_) hardware_->show(frame);
  }

 private:
  gx::SdlSurface& window_;
  gx::ControlSurface* hardware_;
};

void formatStatus(const gx::GroovixApp& app, char* out, size_t size) {
  const gx::UiController& ui = app.ui();
  const gx::Sequencer& seq = app.sequencer();
  const uint8_t track = ui.selectedTrack();
  // Projects and presets as page.slot, e.g. 3.08. A preset also shows the two numbers it
  // actually sends, since a MIDI bank is 128 patches - two pages - and the split is
  // invisible on the pads.
  const uint16_t project = app.currentProject();
  const uint16_t preset = seq.trackPreset(track);
  const bool ownScale = seq.keyboardLayout(track) == gx::kKeyboardOwnScale;
  std::snprintf(out, size,
                "PRJ %d.%02d %-7s TRK %d PAT %d PRE %d.%02d B%d:%d STEP %02d/%d %d BPM "
                "%s %s%s%s OCT %d %s%s",
                project / gx::kSlotsPerPage + 1, project % gx::kSlotsPerPage + 1,
                gx::UiController::modeName(ui.mode()), track + 1, seq.selectedPattern(track) + 1,
                preset / gx::kSlotsPerPage + 1, preset % gx::kSlotsPerPage + 1,
                preset / 128, preset % 128,  // the Bank Select and Program Change it sends
                seq.playhead(track) + 1, seq.trackLength(track), seq.bpm(),
                gx::rootName(ownScale ? seq.keyboardRoot(track) : seq.scaleRoot()),
                gx::scaleName(ownScale ? seq.keyboardScale(track) : seq.scale()),
                kLayoutTags[seq.keyboardLayout(track) % gx::kNumKeyboardLayouts],
                seq.trackPianoRoll(track) ? " ROLL" : "",
                seq.keyboardOctave(track), seq.playing() ? "PLAY" : "STOP",
                seq.recording() ? " REC" : "");
}

int run(const Options& options) {
  // Both come back with the trailing slash on, so paths built from them come out right;
  // FileStorage tidies its own copy the same way.
  const std::string dataDir = gx::rigDataDir(options.dataDir);
  const std::string configDir = gx::rigConfigDir(options.configDir);

#ifndef _WIN32
  // Two locks, because there are two things to clash over. The projects are locked per data
  // directory; the controllers are locked for the machine, since a second simulator would
  // subscribe to the same APC and MIDI Mix and answer every press alongside this one.
  gx::SingleInstance dataLock;
  gx::SingleInstance deviceLock;
  const bool wantsDevices = options.useApc && !options.screenshotPath;
  if (!options.allowMultiple) {
    long otherPid = 0;
    if (!dataLock.take(dataDir + "lock", otherPid)) {
      std::fprintf(stderr, "another GroovixBox simulator");
      if (otherPid > 0) std::fprintf(stderr, " (pid %ld)", otherPid);
      std::fprintf(stderr,
                   " is using these projects.\nClose it first, or start this one with "
                   "--data DIR to keep a separate set.\n");
      return 1;
    }
    if (wantsDevices && !deviceLock.take(gx::SingleInstance::devicePath(), otherPid)) {
      std::fprintf(stderr, "another GroovixBox simulator");
      if (otherPid > 0) std::fprintf(stderr, " (pid %ld)", otherPid);
      std::fprintf(stderr,
                   " is using the controllers.\nClose it first — every press would otherwise "
                   "reach both — or start this one with --no-apc.\n");
      return 1;
    }
  }
#endif

  gx::SdlSurface surface;
  if (!surface.init("GroovixBox Simulator")) return 1;
  for (uint8_t i = 0; i < gx::kNumRightButtons; ++i) {
    surface.setRightLabel(i, gx::UiController::buttonName(i));
  }
  gx::FileStorage storage(dataDir);
  if (!storage.ready()) {
    std::fprintf(stderr, "storage: cannot use '%s' — nothing will be saved\n", dataDir.c_str());
  }
  gx::LogMidiOutput midiLog(options.midiLog ? stdout : NULL);
  gx::MidiOutput* midiOut = &midiLog;
#ifdef GX_HAVE_ALSA
  // The gear around the sequencer: the ports P1..P8, the APC and the MIDI Mix. The same rig
  // the headless build runs, so what is played here is what runs on a box with no screen.
  gx::MidiRig::Options rigOptions;
  rigOptions.useSurfaces = options.useApc && !options.screenshotPath;
  rigOptions.wireOutput = !options.noMidiOut;
  rigOptions.outputDevice = options.midiOutDevice;
  gx::MidiRig rig(rigOptions);
  rig.open();
  TeeMidiOutput tee(rig.output(), midiLog);
  if (rig.outputOpen()) {
    midiOut = options.midiLog ? static_cast<gx::MidiOutput*>(&tee) : &rig.output();
  }
#endif
  gx::MidiEventSink noteOutput(*midiOut);
  // Tracks routed to I1-I16 play here instead of going out of a MIDI port.
  gx::AudioEngine instruments;
  noteOutput.setInstruments(&instruments);

#ifdef GX_HAVE_ALSA
  MirroredSurface controls(surface, &rig);
#else
  MirroredSurface controls(surface, NULL);
#endif


  // On the heap: with the desktop capacity limits the app holds over a megabyte of projects.
  std::unique_ptr<gx::GroovixApp> appOwner(new gx::GroovixApp(controls, storage, noteOutput));
  gx::GroovixApp& app = *appOwner;
  app.begin();

  std::unique_ptr<gx::AudioBackend> audio = openAudio(options, instruments);

  // Which CC each fader and knob sends, and which MIDI device to send out of: the same file
  // carries both, so a rig is set up once rather than on every command line.
  gx::RigConfig config;
  const std::string controlsFile =
      gx::rigConfigPath(options.controlsPath, configDir, "controls.conf");
  // Only complain about a missing file when one was asked for by name or by directory: a rig
  // with neither has simply never made one.
  config.load(app, controlsFile, options.controlsPath != NULL || options.configDir != NULL);
#ifdef GX_HAVE_ALSA
  rig.setOutputName(config.outputName());
  rig.setInputName(config.value("midiin"));
  for (uint8_t port = 0; port < gx::kNumMidiPorts; ++port) {
    rig.setPortSpec(port, config.portSpec(port));
  }
  rig.wire();
  app.setMidiInput(&rig.input());
  app.ui().setDeviceStatus(&rig);
#endif
  gx::InstrumentConfig instrumentConfig;
  loadInstruments(instruments, instrumentConfig,
                  gx::rigConfigPath(options.instrumentsPath, configDir,
                                    "instruments.conf"),
                  options.instrumentsPath != NULL || options.configDir != NULL);
  // Plugins need the device's real rate and block size, so they load once audio is open.
  gx::InstrumentRack rack;
  gx::InstrumentState instrumentState(storage, rack);
  app.setProjectExtras(&instrumentState);
  if (audio) {
    rack.load(instrumentConfig, instruments, audio->sampleRate(), audio->blockSize());
    // The project opened before the plugins existed, so its settings go in now.
    instrumentState.openProject(app.currentProject());
    if (instrumentState.slotsRestored() > 0) {
      std::printf("instruments: %u slot%s restored from the project\n",
                  instrumentState.slotsRestored(),
                  instrumentState.slotsRestored() == 1 ? "" : "s");
    }
  } else if (instrumentConfig.assignedSlots() > 0) {
    std::printf("instruments: no audio device, so no plugins were loaded\n");
  }

  // The sequencer clock ticks every millisecond on its own thread, so playback timing doesn't
  // depend on the frame rate. Core calls from the two threads take turns through coreMutex;
  // drawing happens outside it.
  std::mutex coreMutex;
  gx::ClockThread clock([&](uint32_t nowUs) {
    std::lock_guard<std::mutex> lock(coreMutex);
    app.tick(nowUs);
  });
  clock.start();
  std::printf("clock: 1 ms ticks at %s priority\n", clock.realTime() ? "real-time" : "normal");

  // What the device made of it: both counts should be zero after a session.
  const auto reportAudio = [&audio, &instruments]() {
    if (audio) {
      std::printf("audio: %u xruns, %u events dropped\n", audio->xruns(),
                  instruments.droppedEvents());
      audio->stop();
    }
  };

  char status[128];
  for (int frameCount = 1; !surface.quitRequested(); ++frameCount) {
    {
      std::lock_guard<std::mutex> lock(coreMutex);
#ifdef GX_HAVE_ALSA
      // Gear coming and going: free unless ALSA says something changed. On this thread, never
      // the clock one - taking a surface back up opens ports and allocates.
      rig.pollDevices();
#endif
      app.updateSurface(gx::ClockThread::nowMs());
      // The mixer's bank buttons move the track page, so its strips and the pads agree.
#ifdef GX_HAVE_ALSA
      int8_t bankStep = 0;
      if (rig.takeBankStep(bankStep)) app.ui().stepTrackPage(bankStep);
#endif
      formatStatus(app, status, sizeof(status));
      // The button labels follow what the buttons do, e.g. their Shift functions.
      const gx::UiController& ui = app.ui();
      for (uint8_t i = 0; i < gx::kNumRightButtons; ++i) surface.setRightLabel(i, ui.rightButtonLabel(i));
      for (uint8_t i = 0; i < gx::kNumBottomButtons; ++i) {
        surface.setBottomLabel(i, ui.bottomButtonLabel(i));
      }
      surface.setLabelsShifted(ui.shiftHeld());
      for (uint8_t i = 0; i < gx::kNumPads; ++i) surface.setPadLabel(i, ui.padLabel(i));
    }
    surface.setStatus(status);
    surface.draw();

    if (options.screenshotPath && frameCount >= kScreenshotFrames) {
      clock.stop();
      reportAudio();
      rack.unload(instruments);
      if (!surface.saveScreenshot(options.screenshotPath)) {
        std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
        return 1;
      }
      return 0;  // a screenshot run deliberately leaves the project alone
    }
    surface.present();
  }

  clock.stop();
  reportAudio();
  // The project is saved while the plugins are still loaded: their settings go with it.
  const bool saved = app.saveProject();
  if (saved && instrumentState.slotsSaved() > 0) {
    std::printf("instruments: %u slot%s saved with the project\n", instrumentState.slotsSaved(),
                instrumentState.slotsSaved() == 1 ? "" : "s");
  }
  rack.unload(instruments);
  if (!saved) {
    std::fprintf(stderr, "could not save the project in '%s'\n", dataDir.c_str());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      options.screenshotPath = argv[++i];
    } else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
      options.dataDir = argv[++i];
    } else if (std::strcmp(argv[i], "--controls") == 0 && i + 1 < argc) {
      options.controlsPath = argv[++i];
    } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      options.configDir = argv[++i];
    } else if (std::strcmp(argv[i], "--instruments") == 0 && i + 1 < argc) {
      options.instrumentsPath = argv[++i];
    } else if (std::strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
      options.audioBackend = argv[++i];
    } else if (std::strcmp(argv[i], "--audio-device") == 0 && i + 1 < argc) {
      options.audioDevice = argv[++i];
    } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
      options.sampleRate = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
      options.blockFrames = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--midi-out") == 0 && i + 1 < argc) {
      options.midiOutDevice = argv[++i];
    } else if (std::strcmp(argv[i], "--no-midi-out") == 0) {
      options.noMidiOut = true;
    } else if (std::strcmp(argv[i], "--midi-log") == 0) {
      options.midiLog = true;
    } else if (std::strcmp(argv[i], "--no-apc") == 0) {
      options.useApc = false;
    } else if (std::strcmp(argv[i], "--allow-multiple") == 0) {
      options.allowMultiple = true;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--data DIR] [--config DIR] [--controls FILE]\n"
                   "       [--instruments FILE]\n"
                   "       [--midi-out DEVICE] [--no-midi-out] [--midi-log] [--no-apc]\n"
                   "       [--allow-multiple]\n"
                   "       [--audio none|jack|alsa] [--audio-device NAME] [--rate HZ] "
                   "[--block FRAMES]\n"
                   "       [--screenshot FILE.bmp]\n",
                   argv[0]);
      return 2;
    }
  }

  const int exitCode = run(options);
  SDL_Quit();
  return exitCode;
}
