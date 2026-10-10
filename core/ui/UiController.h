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

// The order is how often a hand reaches for them, nearest first: note mode is where the music
// is written, so it is R1. Nothing may depend on these values matching ModeId - they did once,
// and renderFunctionButtons said so.
//
// The bare presses are the modes that edit the selected track, and R1, R2 and R3 are the three
// that draw the same thing - a page of its steps - so moving between them changes what a pad
// does without changing where anything is. They are next to each other because of it: the hand
// stays put while the lanes under the steps change. What a hand reaches for once a session, not
// once a bar, is on the Shift layer: a project is opened at the start and a setting changed
// when the rig changes, and neither is worth a bare button the step modes can use.
//
// A button's Shift layer is the mode nearest its own, where there is one: scenes sit on
// pattern's button because a scene is which pattern each track plays, and scale on note's
// because a scale is what the keyboard plays.
enum RightButton {
  kButtonNote = 0,     // R1: note mode; Shift + R1 opens scale mode
  kButtonParams,       // R2: step parameters; hold + B1..B8 picks the page; Shift + R2 preset
  kButtonProbability,  // R3: probability mode; Shift + R3 opens global settings
  kButtonPattern,      // R4: pattern mode, or scene mode when it is already open
  kButtonClear,        // R5: hold + pad clears; Shift + R5 opens project mode
  kButtonDuplicate,    // R6: hold + source pad + destination pad copies
  kButtonRecord,       // R7: record on/off
  kButtonPlay,         // R8: play/stop; Shift + R8 opens the song
};

// Pad modes. R1..R4 open note, the step parameters, probability and pattern; Shift + R1..R5
// open scale, preset, global settings, scenes and project. The order here is its own and
// says nothing about which button opens which - see RightButton.
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

  // What a row of page buttons picks in the mode that is open: Shift + B1..B8 and the page
  // panel's rows use this, so a page of what is on screen is one press away. There is no
  // pattern page - pattern mode fits every pattern of its track on the grid at once.
  enum PageKind {
    kPageSteps = 0,  // note, step parameters and probability modes
    kPageProjects,
    kPagePresets,
    kPageTracks,     // everything else: the eight tracks the pads and strips show
  };
  uint8_t pageKind() const;
  // What button 0..7 of a page row picks, false if this build has no such page.
  bool pageRowEntry(uint8_t button, uint8_t& kind, uint8_t& page) const;
  uint8_t pageCount(uint8_t kind) const;
  uint8_t shownPage(uint8_t kind) const;
  // Which 512-voice window preset mode's pages sit inside; the page panel gives it buttons.
  uint8_t presetWindow() const { return state_.presetWindow; }
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
  // Presses button 0..7 of a page row (a page panel row); a button this build has no page
  // for does nothing.
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
  // The page panel: every kind's pages at once, whatever mode is open.
  void renderPagePanel(LedFrame& frame, uint32_t nowMs) const;
  void handlePagePad(uint8_t pad);
  void handlePresetWindow(uint8_t window);
  void handlePanelMode(uint8_t button);
  void handlePanelArrow(uint8_t arrow);
  bool rollArrowsActive() const;
  void clampStepPage();
  // True while R2 is held in preset mode, or Shift is held in project or preset mode: B1..B8
  // pick pages. Project mode has only the Shift gesture - no R button opens it, so there is
  // none to hold there.
  bool pageSelectActive() const;
  // True while Shift is held in any mode whose pads work on the selected track - note,
  // pattern, scale, step parameters, probability, scenes, global settings: B1..B8 pick
  // track pages.
  bool trackPagingActive() const;
  // True while R1 is held in note mode on a piano roll track, the one place this gesture is
  // still needed: B1..B8 pick step pages. Everywhere else Shift + the top pad row does it.
  bool stepPagingActive() const;

  void renderFunctionButtons(LedFrame& frame) const;
  void handleMuteButton(uint8_t strip);
  // Sends a control's CC on its track's channel, if it has one and its value changed. An
  // initial report (a surface's start-up scan) is only recorded.
  void sendControlCc(uint8_t group, uint8_t index, uint16_t position, bool initial);
  // The track a fader or knob belongs to: its strip on the current track page, or the
  // selected track for the master faders.
  uint8_t ccTrack(uint8_t group, uint8_t index) const;
  void renderMixButtons(LedFrame& frame) const;
  bool hasPageData(uint8_t kind, uint8_t page) const;
  void renderTrackButtons(LedFrame& frame) const;
  void renderLibraryPageButtons(LedFrame& frame) const;
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
  bool paramsButtonHeld_;
  bool noteButtonHeld_;
  bool paramsTapPending_;  // R2 pressed in preset mode, not yet used for a page
  bool recordTapPending_;  // R7 pressed in scene mode, not yet used to capture
  bool mixShiftTapPending_;  // the mixer's SOLO is held and hasn't soloed anything yet
};

}  // namespace gx
