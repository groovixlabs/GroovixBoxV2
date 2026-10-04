#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/ControlMap.h"
#include "ui/Controls.h"
#include "ui/ArrangementMode.h"
#include "ui/GlobalMode.h"
#include "ui/LedFrame.h"
#include "ui/Library.h"
#include "ui/Mode.h"
#include "ui/NoteMode.h"
#include "ui/ScaleMode.h"
#include "ui/SceneMode.h"
#include "ui/StepParamsMode.h"
#include "ui/SlotModes.h"
#include "ui/UiState.h"

namespace gx {

enum RightButton {
  kButtonProject = 0,  // R1: project mode; hold + B1..B8 picks the page; Shift + R1 global
  kButtonPattern,      // R2: pattern mode, or scene mode when it is already open
  kButtonNote,         // R3: note mode; Shift + R3 opens scale mode
  kButtonParams,       // R4: step parameters; hold + B1..B8 picks the page; Shift + R4 preset
  kButtonClear,        // R5: hold + pad clears; Shift + R5 probability
  kButtonDuplicate,    // R6: hold + source pad + destination pad copies
  kButtonRecord,       // R7: record on/off
  kButtonPlay,         // R8: play/stop; Shift + R8 opens the song
};

// Pad modes. R1..R4 select the first four; Shift + R3, R4, R1 and R5 open scale, step
// parameters (velocity and gate), global settings and probability.
enum ModeId {
  kModeProject = 0,
  kModePattern,
  kModeNote,
  kModePreset,
  kModeScale,
  kModeStepParams,
  kModeGlobal,
  kModeProbability,
  kModeScene,
  kModeArrangement,
  kNumModes,
};

// Platform-independent UI behaviour: turns control presses into engine and library
// actions and renders their state as LED colours. Never talks to hardware directly.
class UiController {
 public:
  UiController(Sequencer& sequencer, Library& library);

  void handleEvent(const ControlEvent& event);
  void render(LedFrame& frame, uint32_t nowMs) const;
  // Time of the events handled next, so a tap can be told from a hold.
  void setTime(uint32_t nowMs) { state_.nowMs = nowMs; }

  uint8_t mode() const { return mode_; }
  uint8_t selectedTrack() const { return state_.track; }
  // Last reported fader positions, 0..kFaderMax. The faders have no function yet.
  uint16_t faderPosition(uint8_t fader) const {
    return fader < kNumTrackFaders ? state_.faders[fader] : 0;
  }
  uint16_t masterFaderPosition() const { return state_.masterFader; }
  // The mixer's controls, also without a function yet.
  uint16_t knobPosition(uint8_t knob) const { return knob < kNumKnobs ? state_.knobs[knob] : 0; }
  uint16_t mixFaderPosition(uint8_t fader) const {
    return fader < kNumMixStrips ? state_.mixFaders[fader] : 0;
  }
  uint16_t mixMasterPosition() const { return state_.mixMaster; }
  // Which CC each fader and knob sends. Load a config file into it to change the assignments.
  ControlMap& controls() { return controls_; }
  const ControlMap& controls() const { return controls_; }

  // Moves to the track page before or after this one, for a surface with bank buttons. The
  // ends don't wrap: a bank button that does nothing says you are at the end.
  void stepTrackPage(int8_t delta);

  // What a row of page buttons picks in the mode that is open: the mixer's A2 row uses this,
  // so the pages of whatever is on screen are one press away and always visible.
  enum PageKind {
    kPageSteps = 0,  // note, step parameters and probability modes
    kPagePatterns,   // pattern mode
    kPageProjects,
    kPagePresets,
    kPageTracks,     // everything else: the eight tracks the pads and strips show
  };
  uint8_t pageKind() const;
  // What button 0..7 of a page row picks, false if this build has no such page. Pattern mode
  // splits the row: the track pages on B1..B4, the pattern pages on B5..B8.
  bool pageRowEntry(uint8_t button, uint8_t& kind, uint8_t& page) const;
  uint8_t pageCount(uint8_t kind) const;
  uint8_t shownPage(uint8_t kind) const;
  // Sends every control's CC again, at the value it last reported: after a project change the
  // gear has moved on but the faders and knobs have not. Controls nothing has been heard from
  // are left alone, so an untouched knob never slams a synth to zero.
  void sendAllControls();
  // What the platform knows about the gear around it, shown on the devices page of global
  // settings. A build that knows nothing leaves it unset, and the page shows everything absent.
  // Fills a screen's worth of state: what every track is doing while the sequencer runs, and
  // what this mode's rows are for while it is stopped. Called from the drawing thread, like
  // render(); a build with no screen never calls it.
  void fillDisplay(DisplayFrame& frame) const;

  // How many voices each port's gear has, for preset mode's pads and windows.
  void setPresetCatalog(const PresetCatalog* catalog) { presetMode_.setCatalog(catalog); }
  void setDeviceStatus(DeviceStatus* devices) { globalMode_.setDeviceStatus(devices); }
  // A note played on a keyboard plugged into the sequencer. It reaches the selected track
  // whatever is on screen - you can play while looking at the patterns - and while playing and
  // recording it is written at the playhead, velocity and all, exactly as a pad press is.
  void playExternalNote(uint8_t note, uint8_t velocity, bool on);
  // Presses button 0..7 of a page row (the mixer's A2 row, or the APC's bottom row in pattern
  // mode); a button this build has no page for does nothing.
  void selectPage(uint8_t button);
  void applyPage(uint8_t kind, uint8_t page);

  static const char* modeName(uint8_t mode);
  // Function of a right-hand button, e.g. for labels.
  static const char* buttonName(uint8_t button);
  // Labels for what the buttons do right now. While Shift is held they name the Shift function
  // ("" for none), and while a held R button makes B1..B8 page buttons the bottom ones name
  // the pages (PG1..PG8, or the steps a page shows). Otherwise the right buttons give their
  // names and the bottom ones nullptr, leaving them labelled B1..B8.
  const char* rightButtonLabel(uint8_t button) const;
  const char* bottomButtonLabel(uint8_t button) const;
  // Label printed on a pad in the current mode (see Mode::padLabel), or nullptr.
  const char* padLabel(uint8_t pad) const;
  bool shiftHeld() const { return state_.shiftHeld; }

 private:
  void handleButton(uint8_t button, bool pressed);
  void handleTrackButton(uint8_t button);
  void selectMode(uint8_t mode);
  void selectTrack(uint8_t track);
  void selectTrackPage(uint8_t page);
  void selectStepPage(uint8_t page);
  // Shift + a pad of the top row picks that page of whatever the grid shows. True when the
  // row took the pad, so it is never also a pad of the mode - including the matching release,
  // and only that release.
  bool takePageRowPad(uint8_t pad, bool pressed);
  // Which pages the top pad row gives when Shift is held, false for a mode that has none.
  bool pageRowPadKind(uint8_t& kind) const;
  Rgb pageRowPadColor(uint8_t kind, uint8_t page, uint32_t nowMs) const;
  Rgb stepPageColor(uint8_t page, uint32_t nowMs) const;
  void clampStepPage();
  // True while R1 is held in project mode, R4 in preset mode, or Shift in either: B1..B8
  // pick pages.
  bool pageSelectActive() const;
  // True while Shift is held in note or global settings mode: B1..B8 pick track pages.
  bool trackPagingActive() const;
  // True in pattern mode: B1..B4 pick track pages, Shift or not. The pattern pages are on
  // the top pad row under Shift, with the rest of the pages.
  bool patternModePaging() const;
  // True while R3 is held in note mode, R4 in step parameters or R5 in probability mode:
  // B1..B8 pick step pages.
  bool stepPagingActive() const;

  void renderFunctionButtons(LedFrame& frame) const;
  void handleMuteButton(uint8_t strip);
  // Sends a control's CC on its track's channel, if it has one and its value changed. An
  // initial report (a surface's start-up scan) is only recorded.
  void sendControlCc(uint8_t group, uint8_t index, uint16_t position, bool initial);
  // The track a fader or knob belongs to: its strip on the current track page, or the
  // selected track for the master faders.
  uint8_t ccTrack(uint8_t group, uint8_t index) const;
  void renderMixButtons(LedFrame& frame, uint32_t nowMs) const;
  // The mixer's A2 row: the pages of the open mode, with the one you are on brightest.
  void renderMixPageButtons(LedFrame& frame, uint32_t nowMs) const;
  bool hasPageData(uint8_t kind, uint8_t page) const;
  void renderTrackButtons(LedFrame& frame) const;
  void renderLibraryPageButtons(LedFrame& frame) const;
  void renderPatternModeButtons(LedFrame& frame) const;
  void renderTrackPageButtons(LedFrame& frame) const;
  void renderStepPageButtons(LedFrame& frame, uint32_t nowMs) const;

  Sequencer& sequencer_;
  Library& library_;
  UiState state_;
  ProjectMode projectMode_;
  PatternMode patternMode_;
  NoteMode noteMode_;
  PresetMode presetMode_;
  ScaleMode scaleMode_;
  StepParamsMode stepParamsMode_;
  GlobalMode globalMode_;
  StepParamsMode probabilityMode_;
  SceneMode sceneMode_;
  ArrangementMode arrangementMode_;
  Mode* modes_[kNumModes];
  uint8_t mode_;
  ControlMap controls_;
  // Last CC value sent per control, or kNoCc before its first report. Surfaces report every
  // fader and knob once at start-up, and that first report is only recorded, not sent.
  uint8_t lastCc_[kNumCcControls];
  uint16_t mixButtonsHeld_;  // the mixer's A1/A2 buttons currently held
  uint8_t mixSideHeld_;      // the mixer's right-hand column
  uint8_t pageRowHeld_;      // top-row pads whose press the step-page row took
  bool projectButtonHeld_;
  bool paramsButtonHeld_;
  bool noteButtonHeld_;
  bool paramsTapPending_;  // R4 pressed in step parameters mode, not yet used for a page
  bool recordTapPending_;  // R7 pressed in scene mode, not yet used to capture
  bool mixShiftTapPending_;  // the mixer's SOLO is held and hasn't soloed anything yet
};

}  // namespace gx
