#pragma once

#include <stddef.h>
#include <stdint.h>

#include "comm/MidiInput.h"
#include "engine/EventSink.h"
#include "engine/Sequencer.h"
#include "app/ProjectExtras.h"
#include "storage/ProjectCodec.h"
#include "storage/SlotStore.h"
#include "storage/Storage.h"
#include "ui/ControlSurface.h"
#include "ui/LedFrame.h"
#include "ui/Library.h"
#include "ui/UiController.h"

namespace gx {

static const uint16_t kNumProjectSlots = kNumLibrarySlots;  // 8 pages of 64
// A preset is a Bank Select and Program Change for the track's gear, so the slots hold
// nothing here: kNumLibrarySlots of them reach the 512 patches those two messages address.
static const uint16_t kNumPresetSlots = kNumLibrarySlots;

// Wires the portable modules together and provides the UI's Library on top of storage.
// A platform supplies the three interfaces, calls begin() once and update() from its loop.
class GroovixApp : public Library {
 public:
  GroovixApp(ControlSurface& surface, Storage& storage, EventSink& output);

  // Anything the platform keeps beside a project, such as the state of hosted plugins. Set
  // it before begin(), or call its openProject() yourself once whatever it needs is ready.
  void setProjectExtras(ProjectExtras* extras) { extras_ = extras; }
  // Where MIDI comes in from, if anywhere: a keyboard's notes reach the selected track, and
  // its transport keys start and stop the sequencer. Read once a frame, beside the surface.
  void setMidiInput(MidiInput* input) { midiIn_ = input; }

  // Indexes the saved projects and opens the last used one.
  void begin();
  // Handles pending input, runs the sequencer clock and refreshes the LEDs: all of it, for
  // platforms that drive the app from a single loop.
  void update(uint32_t nowMs);
  // For platforms that run the clock separately (a timer thread, or a fast MCU loop): tick()
  // advances the sequencer and should be called as often as possible, while updateSurface()
  // handles input and refreshes the LEDs at frame rate. Calls must never overlap.
  void tick(uint32_t nowUs);
  void updateSurface(uint32_t nowMs);
  // Writes the current project to its slot. An empty project frees the slot.
  bool saveProject();
  // Applies a control config file, which says which CC each fader and knob sends (see
  // ControlMap). Without one the built-in defaults stay. On a bad line it returns false and
  // reports the 1-based line in errorLine.
  bool loadControlConfig(const char* text, size_t length, uint16_t* errorLine = NULL) {
    return ui_.controls().load(text, length, errorLine);
  }

  const Sequencer& sequencer() const { return sequencer_; }
  const UiController& ui() const { return ui_; }
  UiController& ui() { return ui_; }

  // Library
  uint16_t currentProject() const override { return currentProject_; }
  bool projectHasData(uint16_t slot) const override;
  bool projectPageHasData(uint8_t page) const override;
  void selectProject(uint16_t slot) override;
  void clearProject(uint16_t slot) override;
  void copyProject(uint16_t from, uint16_t to) override;

 private:
  void handleInput(uint32_t nowMs);
  void handleMidiInput(uint32_t nowUs);
  // Whether to be following right now: the source says, and Auto also wants a clock that is
  // actually arriving.
  void updateClockFollowing(uint32_t nowUs);
  void refreshLeds(uint32_t nowMs);
  void openProject(uint16_t slot);
  bool loadProjectSlot(uint16_t slot);
  void resetProject();

  ControlSurface& surface_;
  Storage& storage_;
  ProjectExtras* extras_;  // NULL when the platform keeps nothing beside its projects
  MidiInput* midiIn_;      // NULL when nothing can be played into it
  uint32_t lastClockUs_;   // when a MIDI clock last arrived, for the Auto source
  bool clockSeen_;
  SlotStore projects_;
  Sequencer sequencer_;
  UiController ui_;
  LedFrame frame_;
  uint16_t currentProject_;
  // Working buffers kept as members: they are too large for a small MCU stack.
  Project scratchProject_;
  uint8_t projectBuffer_[kMaxEncodedProjectSize];
};

}  // namespace gx
