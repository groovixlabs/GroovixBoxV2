// GroovixBox on a Linux box with no screen: the sequencer, its MIDI ports and the hardware
// surfaces, and nothing else. This is what runs on a Raspberry Pi in a case - the same core
// and the same platform code as the simulator, without the window.
//
//   groovix [--data DIR] [--config DIR] [--controls FILE] [--midi-out DEVICE]
//           [--no-midi-out] [--midi-log] [--no-surfaces] [--allow-multiple]
//
//   --data DIR        where projects and presets are kept
//                     (default: $GXBOX_DATA_DIR, else /mnt/usb1/data)
//   --config DIR      where controls.conf is read from, so the rig's settings can live apart
//                     from its projects - in /etc, beside your dotfiles, or on read-only
//                     media (default: $GXBOX_CONFIG_DIR, else /mnt/usb1/config)
//   --controls FILE   the rig's config file, named outright
//                     (default: controls.conf in the config directory)
//   --midi-out DEVICE wire P1..P8 to this device, overriding the config
//   --no-midi-out     leave the ports unwired; patch them by hand
//   --midi-log        print every MIDI message the sequencer sends
//   --no-surfaces     don't open the APC or the MIDI Mix
//   --no-display      leave the 800x480 panel shut (only in a GX_BUILD_DISPLAY=ON build)
//   --allow-multiple  run beside another instance (they share the gear; for testing)
//
// It runs until Ctrl-C or SIGTERM, then saves the open project and puts the LEDs out, so
// "systemctl stop" leaves a clean instrument behind.

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "app/GroovixApp.h"
#include "comm/MidiEventSink.h"
#include "common/ClockThread.h"
#include "common/FileStorage.h"
#include "common/LogMidiOutput.h"
#include "common/RigPaths.h"
#include "common/VoiceLibrary.h"
#ifdef GX_HAVE_DISPLAY
#include "display/DisplayWindow.h"
#endif
#include "linux/MidiRig.h"
#include "linux/RigConfig.h"
#include "linux/SingleInstance.h"

namespace {

std::atomic<bool> gRunning(true);

#ifdef GX_HAVE_DISPLAY
// The display shows voices by name; the voice lists are what has them.
class LibraryNaming : public gx::DisplayWindow::Naming {
 public:
  explicit LibraryNaming(const gx::VoiceLibrary& voices) : voices_(voices) {}
  std::string voiceName(uint8_t port, uint16_t slot) const override {
    return port == gx::kNoDisplayPort ? std::string() : voices_.voiceName(port, slot);
  }

 private:
  const gx::VoiceLibrary& voices_;
};
#endif
void stopOnSignal(int) { gRunning = false; }

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

struct Options {
  const char* dataDir = NULL;
  bool useDisplay = true;  // only reaches anything in a build with GX_BUILD_DISPLAY=ON
  const char* configDir = NULL;
  const char* controlsPath = NULL;
  const char* midiOutDevice = NULL;
  bool noMidiOut = false;
  bool midiLog = false;
  bool useSurfaces = true;
  bool allowMultiple = false;
};

int run(const Options& options) {
  const std::string dataDir = gx::rigDataDir(options.dataDir);
  const std::string configDir = gx::rigConfigDir(options.configDir);
  gx::SingleInstance lock;
  if (!options.allowMultiple) {
    long otherPid = 0;
    if (!lock.take(gx::SingleInstance::devicePath(), otherPid)) {
      std::fprintf(stderr,
                   "GroovixBox is already running (pid %ld). Two of them share the APC and the\n"
                   "MIDI Mix and both answer every press — stop that one first.\n", otherPid);
      return 1;
    }
  }

  gx::FileStorage storage(dataDir);
  if (!storage.ready()) {
    std::fprintf(stderr, "storage: cannot use '%s' — nothing will be saved\n", dataDir.c_str());
  }

  gx::MidiRig::Options rigOptions;
  rigOptions.useSurfaces = options.useSurfaces;
  rigOptions.wireOutput = !options.noMidiOut;
  rigOptions.outputDevice = options.midiOutDevice;
  gx::MidiRig rig(rigOptions);
  rig.open();

  gx::LogMidiOutput midiLog(options.midiLog ? stdout : NULL);
  TeeMidiOutput tee(rig.output(), midiLog);
  gx::MidiOutput& midiOut = options.midiLog ? static_cast<gx::MidiOutput&>(tee)
                                            : rig.output();
  gx::MidiEventSink noteOutput(midiOut);

  // What each device's presets actually send. Read before the ports are wired, so the first
  // wiring already knows which list belongs to which port.
  gx::VoiceLibrary voices;
  voices.load(configDir, stdout);
  noteOutput.setVoices(&voices);
  midiLog.setVoices(&voices);
  rig.setVoiceLibrary(&voices);

  // On the heap: with the desktop capacity limits the app holds over a megabyte of projects.
  std::unique_ptr<gx::GroovixApp> appOwner(new gx::GroovixApp(rig, storage, noteOutput));
  gx::GroovixApp& app = *appOwner;

  gx::RigConfig config;
  // Only complain about a missing file when one was asked for by name or by directory: a rig
  // with neither has simply never made one.
  config.load(app, gx::rigConfigPath(options.controlsPath, configDir, "controls.conf"),
              options.controlsPath != NULL || options.configDir != NULL);
  // Where the clock and the transport may go is the sink's business, not the rig's, and the
  // two are asked for separately: a port can be kept out of either without losing the other.
  noteOutput.setTransportPorts(config.transportPorts());
  noteOutput.setClockPorts(config.clockPorts());
  rig.setOutputName(config.outputName());
  rig.setInputName(config.value("midiin"));
  // Which device plays which role, then the look for them: the roles decide which devices
  // are opened at all, so they are set first.
  rig.setRoleDevice(gx::MidiRig::kRoleGrid, config.gridDevice());
  rig.setRoleDevice(gx::MidiRig::kRolePanel, config.panelDevice());
  rig.setRoleDevice(gx::MidiRig::kRoleMixer, config.mixerDevice());
  rig.openSurfaces();
  for (uint8_t port = 0; port < gx::kNumMidiPorts; ++port) {
    rig.setPortSpec(port, config.portSpec(port));
    rig.setPortSocket(port, config.portSocket(port));
  }
  rig.wire();
  for (uint8_t port = 0; port < gx::kNumMidiPorts; ++port) {
    std::printf("voices: %s\n", voices.describePort(port).c_str());
  }
  app.setMidiInput(&rig.input());
  app.ui().setDeviceStatus(&rig);
  app.ui().setPresetCatalog(&voices);

  // Only now: opening a project sends every track's preset, and those have to go out of ports
  // that are wired, to the voice list of the gear that is on them. Begun before the wiring,
  // they went nowhere and were read against whatever list the port had yet to be given.
  app.begin();

  // The clock ticks on its own thread so playback timing doesn't depend on how often the
  // surfaces are polled. The two threads take turns through one mutex, as the simulator does.
  std::mutex coreMutex;
  gx::ClockThread clock([&app, &coreMutex](uint32_t nowUs) {
    std::lock_guard<std::mutex> held(coreMutex);
    app.tick(nowUs);
  });
#ifdef GX_HAVE_DISPLAY
  // The panel beside the instrument. It is a reporter, not a control: nothing it shows can be
  // touched, and a rig without one runs exactly the same.
  gx::DisplayWindow display;
  LibraryNaming naming(voices);
  if (options.useDisplay && display.open(config.value("displayfont"))) {
    display.setNaming(&naming);
  }
  gx::DisplayFrame displayFrame;
  uint32_t lastDraw = 0;
#endif

  clock.start();
  std::printf("clock: 1 ms ticks at %s priority\n", clock.realTime() ? "real-time" : "normal");
  std::printf("GroovixBox running. Ctrl-C to stop.\n");
  std::fflush(stdout);

  // Input and LEDs at about 60 Hz: fast enough for the pads to feel immediate, slow enough to
  // leave the machine alone.
  bool drawPending = false;
  (void)drawPending;
  while (gRunning) {
    {
      std::lock_guard<std::mutex> held(coreMutex);
      // Gear coming and going: free unless ALSA says something changed. On this thread, never
      // the clock one - taking a surface back up opens ports and allocates.
      rig.pollDevices();
      app.updateSurface(gx::ClockThread::nowMs());
      // The mixer's bank buttons move the track page, so its strips and the pads agree.
      int8_t bankStep = 0;
      if (rig.takeBankStep(bankStep)) app.ui().stepTrackPage(bankStep);
#ifdef GX_HAVE_DISPLAY
      // 30 a second is plenty for something read at arm's length, and it halves the drawing
      // this thread does while the pads still answer at 60.
      const uint32_t now = gx::ClockThread::nowMs();
      if (display.isOpen() && now - lastDraw >= 33) {
        lastDraw = now;
        app.ui().fillDisplay(displayFrame);
        drawPending = true;
      }
#endif
    }
#ifdef GX_HAVE_DISPLAY
    // Drawn outside the lock: pixels take long enough that the clock thread should not be
    // kept waiting on them, and the frame is this thread's own copy by now.
    if (drawPending) {
      display.show(displayFrame);
      drawPending = false;
    }
#endif
    usleep(16000);
  }

  clock.stop();
  std::printf("\nsaving…\n");
  app.saveProject();
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
      options.dataDir = argv[++i];
    } else if (std::strcmp(argv[i], "--controls") == 0 && i + 1 < argc) {
      options.controlsPath = argv[++i];
    } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      options.configDir = argv[++i];
    } else if (std::strcmp(argv[i], "--midi-out") == 0 && i + 1 < argc) {
      options.midiOutDevice = argv[++i];
    } else if (std::strcmp(argv[i], "--no-midi-out") == 0) {
      options.noMidiOut = true;
    } else if (std::strcmp(argv[i], "--midi-log") == 0) {
      options.midiLog = true;
    } else if (std::strcmp(argv[i], "--no-surfaces") == 0) {
      options.useSurfaces = false;
    } else if (std::strcmp(argv[i], "--no-display") == 0) {
      options.useDisplay = false;
    } else if (std::strcmp(argv[i], "--allow-multiple") == 0) {
      options.allowMultiple = true;
    } else {
      std::fprintf(stderr,
                   "usage: %s [--data DIR] [--config DIR] [--controls FILE]\n"
                   "       [--midi-out DEVICE] [--no-midi-out] [--midi-log] [--no-surfaces]\n"
                   "       [--allow-multiple]\n",
                   argv[0]);
      return 2;
    }
  }
  signal(SIGINT, stopOnSignal);
  signal(SIGTERM, stopOnSignal);
  return run(options);
}
