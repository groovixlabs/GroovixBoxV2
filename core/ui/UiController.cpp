#include "ui/UiController.h"

#include "engine/FactoryPatterns.h"
#include "ui/Palette.h"

namespace gx {

static_assert(kButtonPlay < kNumRightButtons, "every function needs a button");
static_assert(kNumBottomButtons == kTracksPerPage, "track buttons sit under the pad columns");
static_assert(kNumTrackPages <= kNumBottomButtons,
              "the track pages have to fit on B1..B8");
static_assert(kNumPatterns <= kNumPads,
              "pattern mode gives every pattern of a track a pad, so they all have to fit");
static_assert(kNumStepPages <= kGridCols,
              "the step pages have to fit the top pad row under Shift");

namespace {

const char* const kModeNames[kNumModes] = {"PROJECT", "PATTERN", "NOTE",  "PRESET", "SCALE",
                                           "PARAMS",  "GLOBAL",  "PROB",  "SCENE",  "SONG"};

// In RightButton order, which is the order of the physical row.
const char* const kButtonNames[kNumRightButtons] = {
    "NOTE", "PARAMS", "PROB", "PATTERN", "CLEAR", "DUPLICATE", "RECORD", "PLAY",
};

const Rgb kButtonColors[kNumRightButtons] = {
    {255, 255, 255},  // R1 note
    {255, 255, 255},  // R2 params
    {255, 255, 255},  // R3 probability
    {255, 255, 255},  // R4 pattern
    {255, 90, 0},     // R5 clear
    {0, 150, 255},    // R6 duplicate
    {255, 0, 0},      // R7 record
    {0, 255, 0},      // R8 play
};

// Labels for Shift + a button. R6 and R7 are blank because the APC's firmware keeps them for
// its Drum and Note modes.
const char* const kShiftRightLabels[kNumRightButtons] = {"SCALE",   "PRESET", "GLOBAL", "SCENE",
                                                         "PROJECT", "",       "",       "SONG"};
const char* const kTrackPageLabels[kNumBottomButtons] = {"T1-8",   "T9-16",  "T17-24", "T25-32",
                                                         "T33-40", "T41-48", "T49-56", "T57-64"};
// Labels for B1..B8 while a held R button makes them page buttons.
const char* const kSlotPageLabels[kNumBottomButtons] = {"PG1", "PG2", "PG3", "PG4",
                                                        "PG5", "PG6", "PG7", "PG8"};
const char* const kStepPageLabels[kNumBottomButtons] = {"S1-32",    "S33-64",   "S65-96",
                                                        "S97-128",  "S129-160", "S161-192",
                                                        "S193-224", "S225-256"};
static_assert(kStepsPerPage == 32, "the step page labels count 32 steps a page");
static_assert(kNumPages <= kNumBottomButtons, "every project and preset page has a label");
// The page panel has a row per kind of page and a pad per page of it.
static_assert(UiController::kPageTracks + 1 == kNumPageKinds,
              "a page panel row for every PageKind");
static_assert(kNumPanelWindows == kPagesPerKind,
              "the window row is a row like any other, so it holds every window the model has");
// The arrow buttons are passed straight to PianoRoll::scroll, so their order is RollScroll's:
// up, down, left, right. Nothing translates between them, and this is what says so.
static_assert(kNumPanelArrows == kNumRollScrolls, "an arrow button per roll direction");
static_assert(kRollUp == 0 && kRollDown == 1 && kRollLeft == 2 && kRollRight == 3,
              "arrow buttons 1..4 are up, down, left and right in that order");
static_assert(kNumStepPages <= kPagesPerKind && kNumTrackPages <= kPagesPerKind &&
                  kNumPages <= kPagesPerKind,
              "every page of every kind reaches a pad of the page panel");

// Which kind of page each row of the page panel shows, top to bottom. This is the player's
// layout, deliberately not the order the kinds are declared in: steps and tracks are what
// you reach for while playing, so they take the top rows, and presets - the row with the
// window buttons beside it - takes the bottom.
const uint8_t kPanelRowKind[kNumPageKinds] = {
    UiController::kPageSteps, UiController::kPageTracks, UiController::kPageProjects,
    UiController::kPagePresets};

// Which mode each of the panel's mode buttons opens, top to bottom: scale, project, scenes,
// the settings, preset. These are exactly the five that need Shift on the APC, so the panel
// reaches every one of them in a single press. Probability was here while it was Shift + R5;
// it has a bare button of its own now, and the button it vacated went to project, which does
// not - the set follows the Shift layer rather than being a list of its own.
const uint8_t kPanelModeOf[kNumPanelModes] = {kModeScale, kModeProject, kModeScene,
                                              kModeGlobal, kModePreset};

const uint8_t kIdleLevel = 40;         // buttons that are not active
const uint8_t kSilencedLevel = 130;    // a mixer strip another track's solo has silenced

uint8_t pageOf(uint16_t slot) {
  const uint16_t page = slot / kSlotsPerPage;
  return page < kNumPages ? static_cast<uint8_t>(page) : 0;
}

// Page buttons: green = page shown, dim blue = page with data, grey = empty, off = no page.
Rgb pageColor(uint8_t page, uint8_t shown, uint8_t numPages, bool hasData) {
  if (page >= numPages) return kBlack;
  if (page == shown) return kSelectedColor;
  return hasData ? kFilledColor : kEmptyColor;
}

// Whether any pattern in the given ranges of tracks and patterns has steps.
bool anyPatternData(const Sequencer& sequencer, uint16_t firstTrack, uint16_t numTracks,
                    uint16_t firstPattern, uint16_t numPatterns) {
  for (uint16_t t = firstTrack; t < firstTrack + numTracks && t < kNumTracks; ++t) {
    for (uint16_t p = firstPattern; p < firstPattern + numPatterns && p < kNumPatterns; ++p) {
      if (sequencer.patternHasData(static_cast<uint8_t>(t), static_cast<uint8_t>(p))) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

UiController::UiController(Sequencer& sequencer, Library& library)
    : sequencer_(sequencer),
      library_(library),
      projectMode_(library),
      patternMode_(sequencer),
      noteMode_(sequencer),
      presetMode_(sequencer),
      scaleMode_(sequencer),
      stepParamsMode_(sequencer, kLaneVelocity, kLaneGate, kLaneRatchet),
      globalMode_(sequencer),
      probabilityMode_(sequencer, kLaneProbability, kLaneNudge),
      sceneMode_(sequencer),
      arrangementMode_(sequencer),
      mode_(kModeNote),
      mixButtonsHeld_(0),
      mixSideHeld_(0),
      pageRowHeld_(0),
      paramsButtonHeld_(false),
      noteButtonHeld_(false),
      paramsTapPending_(false),
      recordTapPending_(false),
      mixShiftTapPending_(false) {
  state_.track = 0;
  state_.stepPage = 0;
  state_.trackPage = 0;
  state_.projectPage = 0;
  state_.presetPage = 0;
  state_.presetWindow = 0;
  state_.shiftHeld = false;
  state_.noteHeld = false;
  state_.clearHeld = false;
  state_.duplicateHeld = false;
  state_.recordHeld = false;
  state_.duplicateSource = kNoSlot;
  for (uint8_t i = 0; i < kNumTrackFaders; ++i) state_.faders[i] = 0;
  state_.masterFader = 0;
  for (uint8_t i = 0; i < kNumCcControls; ++i) lastCc_[i] = kNoCc;
  for (uint8_t i = 0; i < kNumKnobs; ++i) state_.knobs[i] = 0;
  for (uint8_t i = 0; i < kNumMixStrips; ++i) state_.mixFaders[i] = 0;
  state_.mixMaster = 0;
  state_.nowMs = 0;
  state_.padVelocity = 0;
  state_.playingNoteCount = 0;
  state_.sendAllRequested = false;
  modes_[kModeProject] = &projectMode_;
  modes_[kModePattern] = &patternMode_;
  modes_[kModeNote] = &noteMode_;
  modes_[kModePreset] = &presetMode_;
  modes_[kModeScale] = &scaleMode_;
  modes_[kModeStepParams] = &stepParamsMode_;
  modes_[kModeGlobal] = &globalMode_;
  modes_[kModeProbability] = &probabilityMode_;
  modes_[kModeScene] = &sceneMode_;
  modes_[kModeArrangement] = &arrangementMode_;
}

// A keyboard's notes belong to the track, not to the mode: they sound and record the same
// whether the grid is showing steps, patterns or the song.
void UiController::playExternalNote(uint8_t note, uint8_t velocity, bool on) {
  if (note > kMaxMidiValue) return;
  if (!on) {
    sequencer_.previewNoteOff(state_.track, note);
    for (uint8_t i = 0; i < state_.playingNoteCount; ++i) {
      if (state_.playingNotes[i] != note) continue;
      state_.playingNotes[i] = state_.playingNotes[--state_.playingNoteCount];
      break;
    }
    return;
  }
  sequencer_.previewNoteOn(state_.track, note, velocity);
  sequencer_.recordNote(state_.track, note, velocity);
  // Remember it for the grid. A hand is smaller than the list, and if it isn't, the oldest
  // note stops being drawn rather than the newest never appearing.
  for (uint8_t i = 0; i < state_.playingNoteCount; ++i) {
    if (state_.playingNotes[i] == note) return;
  }
  if (state_.playingNoteCount == kMaxPlayingNotes) {
    for (uint8_t i = 1; i < kMaxPlayingNotes; ++i) state_.playingNotes[i - 1] = state_.playingNotes[i];
    --state_.playingNoteCount;
  }
  state_.playingNotes[state_.playingNoteCount++] = note;
}

void UiController::handleEvent(const ControlEvent& event) {
  switch (event.group) {
    case kGroupPad:
      if (event.index < kNumPads) {
        // A pad pressed while Record is held used it as a modifier, so releasing R7 then
        // shouldn't also start recording.
        if (event.pressed && state_.recordHeld) recordTapPending_ = false;
        // Shift + the top pad row picks a page. Handled here rather than in each of the
        // modes that have pages, so all of them behave the same.
        if (takePageRowPad(event.index, event.pressed)) break;
        state_.padVelocity = event.velocity;  // for the one call, then back to nothing
        modes_[mode_]->handlePad(state_, event.index, event.pressed);
        state_.padVelocity = 0;
        // A pad can ask for something only the controller can do.
        if (state_.sendAllRequested) {
          state_.sendAllRequested = false;
          sendAllControls();
        }
      }
      break;
    case kGroupRight:
      if (event.index < kNumRightButtons) handleButton(event.index, event.pressed);
      break;
    case kGroupBottom:
      if (event.pressed && event.index < kNumBottomButtons) handleTrackButton(event.index);
      break;
    case kGroupShift:
      state_.shiftHeld = event.pressed;
      break;
    case kGroupPage:
      if (event.pressed) handlePagePad(event.index);
      break;
    case kGroupPresetWindow:
      if (event.pressed) handlePresetWindow(event.index);
      break;
    case kGroupPanelMode:
      if (event.pressed) handlePanelMode(event.index);
      break;
    case kGroupPanelArrow:
      if (event.pressed) handlePanelArrow(event.index);
      break;
    case kGroupFader:
      if (event.index < kNumTrackFaders) {
        state_.faders[event.index] = event.value > kFaderMax ? kFaderMax : event.value;
        // A fader the mode uses, like the tempo fader, doesn't also send its CC.
        if (!modes_[mode_]->handleFader(state_, event.index, state_.faders[event.index])) {
          sendControlCc(kGroupFader, event.index, state_.faders[event.index], event.initial);
        }
      }
      break;
    case kGroupMasterFader:
      state_.masterFader = event.value > kFaderMax ? kFaderMax : event.value;
      sendControlCc(kGroupMasterFader, 0, state_.masterFader, event.initial);
      break;
    // The mixer's knobs and faders send their CCs; its A2 row and side buttons only light
    // while they are held.
    case kGroupKnob:
      if (event.index < kNumKnobs) {
        state_.knobs[event.index] = event.value > kFaderMax ? kFaderMax : event.value;
        sendControlCc(kGroupKnob, event.index, state_.knobs[event.index], event.initial);
      }
      break;
    case kGroupMixFader:
      if (event.index < kNumMixStrips) {
        state_.mixFaders[event.index] = event.value > kFaderMax ? kFaderMax : event.value;
        sendControlCc(kGroupMixFader, event.index, state_.mixFaders[event.index], event.initial);
      }
      break;
    case kGroupMixMaster:
      state_.mixMaster = event.value > kFaderMax ? kFaderMax : event.value;
      sendControlCc(kGroupMixMaster, 0, state_.mixMaster, event.initial);
      break;
    case kGroupMixButton:
      if (event.index < kNumMixButtons) {
        const uint16_t bit = static_cast<uint16_t>(1u << event.index);
        mixButtonsHeld_ = static_cast<uint16_t>(event.pressed ? (mixButtonsHeld_ | bit)
                                                              : (mixButtonsHeld_ & ~bit));
        if (event.pressed) mixShiftTapPending_ = false;  // SOLO was used as a modifier
        // The A1 row mutes. The A2 row (REC ARM on a MIDI Mix) does nothing: it is held and
        // released like any button, and lights while it is held, but nothing acts on it.
        if (event.pressed && event.index < kNumMixStrips) handleMuteButton(event.index);
      }
      break;
    case kGroupMixSide:
      if (event.index < kNumMixSideButtons) {
        const uint8_t bit = static_cast<uint8_t>(1u << event.index);
        mixSideHeld_ = static_cast<uint8_t>(event.pressed ? (mixSideHeld_ | bit)
                                                          : (mixSideHeld_ & ~bit));
        // Held, SOLO turns the mute buttons into solo buttons. Tapped on its own it drops
        // every solo, which is how you get back to hearing everything without hunting for
        // the tracks you soloed.
        if (event.index == kMixShiftButton) {
          if (event.pressed) {
            mixShiftTapPending_ = true;
          } else if (mixShiftTapPending_) {
            mixShiftTapPending_ = false;
            sequencer_.clearSolos();
          }
        }
      }
      break;
  }
}

const char* UiController::rightButtonLabel(uint8_t button) const {
  if (button >= kNumRightButtons) return "";
  if (!state_.shiftHeld) return kButtonNames[button];
  return kShiftRightLabels[button];
}

const char* UiController::bottomButtonLabel(uint8_t button) const {
  if (button >= kNumBottomButtons) return nullptr;
  // In the order handleTrackButton checks them.
  if (pageSelectActive()) return button < kNumPages ? kSlotPageLabels[button] : "";
  if (stepPagingActive()) return button < kNumStepPages ? kStepPageLabels[button] : "";
  if (!state_.shiftHeld) return nullptr;
  if (trackPagingActive()) return button < kNumTrackPages ? kTrackPageLabels[button] : "";
  return "";
}

const char* UiController::padLabel(uint8_t pad) const {
  if (pad >= kNumPads) return nullptr;
  // While Shift paints the pages over the top row, name the pages rather than what the mode
  // drew underneath - the pad does the one and not the other.
  uint8_t kind = 0;
  if (state_.shiftHeld && pad < kGridCols && pageRowPadKind(kind)) {
    return pad < pageCount(kind) ? kStepPageLabels[pad] : "";
  }
  return modes_[mode_]->padLabel(state_, pad);
}

void UiController::handleButton(uint8_t button, bool pressed) {
  // Shift + R1 opens scale mode everywhere else; R1 alone returns to note mode.
  if (pressed && button == kButtonNote && state_.shiftHeld) {
    selectMode(kModeScale);
    return;
  }
  // Shift + R2 opens preset mode; R2 on its own is the step parameters, which are edited far
  // more often than a patch is picked.
  if (pressed && button == kButtonParams && state_.shiftHeld) {
    selectMode(kModePreset);
    return;
  }
  // Shift + R3 opens global settings; R3 alone goes to probability mode.
  if (pressed && button == kButtonProbability && state_.shiftHeld) {
    selectMode(kModeGlobal);
    return;
  }
  // Shift + R4 opens scene mode; R4 alone goes to pattern mode as usual.
  if (pressed && button == kButtonPattern && state_.shiftHeld) {
    selectMode(kModeScene);
    return;
  }
  // Shift + R8 opens the song: R8 is Play, so shift-play plays the song. R6 and R7 are not
  // ours to use — the APC's own firmware takes those two for its Drum and Note pad modes,
  // and never passes them on.
  if (pressed && button == kButtonPlay && state_.shiftHeld) {
    selectMode(kModeArrangement);
    return;
  }
  // In scene mode R7 is held to capture with a pad, so it acts on release: a tap that
  // captured nothing still starts or stops recording.
  if (mode_ == kModeScene && button == kButtonRecord) {
    state_.recordHeld = pressed;
    if (pressed) {
      recordTapPending_ = true;
    } else if (recordTapPending_) {
      recordTapPending_ = false;
      sequencer_.setRecording(!sequencer_.recording());
    }
    return;
  }
  // Shift + R5 opens project mode; R5 on its own stays the Clear modifier.
  if (pressed && button == kButtonClear && state_.shiftHeld) {
    selectMode(kModeProject);
    return;
  }
  // In preset mode R2 is held to pick a preset page with B1..B8, so it acts on release: a tap
  // that picked no page opens the step parameters as usual.
  if (mode_ == kModePreset && button == kButtonParams) {
    paramsButtonHeld_ = pressed;
    if (pressed) {
      paramsTapPending_ = true;
    } else if (paramsTapPending_) {
      paramsTapPending_ = false;
      selectMode(kModeStepParams);
    }
    return;
  }

  switch (button) {
    case kButtonProbability:
      if (pressed) selectMode(kModeProbability);
      break;
    case kButtonPattern:
      if (pressed) selectMode(kModePattern);
      break;
    case kButtonNote:
      noteButtonHeld_ = pressed;
      state_.noteHeld = pressed;
      if (pressed) selectMode(kModeNote);
      break;
    case kButtonParams:
      paramsButtonHeld_ = pressed;
      if (pressed) selectMode(kModeStepParams);
      break;
    case kButtonClear:
      state_.clearHeld = pressed;
      break;
    case kButtonDuplicate:
      state_.duplicateHeld = pressed;
      state_.duplicateSource = kNoSlot;
      break;
    case kButtonRecord:
      if (pressed) sequencer_.setRecording(!sequencer_.recording());
      break;
    case kButtonPlay:
      if (pressed) {
        if (sequencer_.playing()) {
          sequencer_.stop();
        } else {
          // Play pressed in song mode plays the song: it runs the arrangement from where it
          // stands. Anywhere else it is the plain transport, a loop of whatever is selected.
          if (mode_ == kModeArrangement) sequencer_.startSong();
          sequencer_.play();
        }
      }
      break;
  }
}

// B1..B8 normally return to note mode on a track of the current track page; in pattern mode
// and global settings they change the track without leaving the mode, so a pattern, channel,
// port or instrument can be given to one track after another. With Shift in project or preset
// mode (or R2 held in preset mode) they pick a project/preset page, and with Shift in any
// mode that works on the selected track a track page. Changing track or page keeps a picked
// Duplicate source, so patterns can be copied between tracks and items between pages.
void UiController::handleTrackButton(uint8_t button) {
  if (pageSelectActive()) {
    if (mode_ == kModeProject) {
      state_.projectPage = button;
    } else {
      paramsTapPending_ = false;  // R2 was used for a page, so its release stays in the mode
      state_.presetPage = button;
    }
  } else if (stepPagingActive()) {
    if (button < kNumStepPages) selectStepPage(button);
  } else if (trackPagingActive()) {
    if (button < kNumTrackPages) selectTrackPage(button);
  } else {
    const uint16_t track = state_.trackPage * kTracksPerPage + button;
    if (track < kNumTracks) selectTrack(static_cast<uint8_t>(track));
  }
}

// How many pages of a kind this build has.
uint8_t UiController::pageCount(uint8_t kind) const {
  switch (kind) {
    case kPageSteps:
      return kNumStepPages;
    case kPageTracks:
      return kNumTrackPages;
    default:
      return kNumPages;
  }
}

// Which page of that kind is on screen.
uint8_t UiController::shownPage(uint8_t kind) const {
  switch (kind) {
    case kPageSteps:
      return state_.stepPage;
    case kPageProjects:
      return state_.projectPage;
    case kPagePresets:
      return state_.presetPage;
    default:
      return state_.trackPage;
  }
}

// What button 0..7 of a page row picks: one mapping for every mode, used by Shift + B1..B8
// and by every row of the page panel. False for a button this build has no page for.
bool UiController::pageRowEntry(uint8_t button, uint8_t& kind, uint8_t& page) const {
  kind = pageKind();
  page = button;
  return page < pageCount(kind);
}

// ---- a screen beside the instrument ----

// While the sequencer runs you want to know what every track is doing; while it is stopped
// you are editing, and what helps is what this mode's rows are for. Only the middle changes
// between the two - the transport above and the values below stay put, so the screen never
// rearranges itself under you.
void UiController::fillDisplay(DisplayFrame& frame) const {
  frame.bpm = sequencer_.followingExternal() ? sequencer_.externalBpm() : sequencer_.bpm();
  frame.clockSource = sequencer_.clockSource();
  frame.followingExternal = sequencer_.followingExternal();
  frame.playing = sequencer_.playing();
  frame.recording = sequencer_.recording();
  frame.project = library_.currentProject();

  frame.showTracks = sequencer_.playing();
  frame.numTracks = 0;
  if (frame.showTracks) {
    const uint8_t first = static_cast<uint8_t>(state_.trackPage * kTracksPerPage);
    for (uint8_t i = 0; i < kTracksPerPage; ++i) {
      const uint16_t track = static_cast<uint16_t>(first + i);
      if (track >= kNumTracks) break;
      const uint8_t t = static_cast<uint8_t>(track);
      DisplayTrack& line = frame.tracks[frame.numTracks++];
      line.color = trackColor(t);
      line.number = static_cast<uint8_t>(t + 1);
      const uint8_t instrument = sequencer_.trackInstrument(t);
      line.instrument = instrument == kNoInstrument ? kNoDisplayPort : instrument;
      line.port = instrument == kNoInstrument ? sequencer_.trackMidiPort(t) : kNoDisplayPort;
      line.channel = sequencer_.trackMidiChannel(t);
      line.preset = sequencer_.trackPreset(t);
      line.pattern = static_cast<uint8_t>(sequencer_.selectedPattern(t) + 1);
      line.muted = sequencer_.trackMuted(t);
      line.soloed = sequencer_.trackSoloed(t);
      // The four steps up to the playhead: what the track has just played, which is what a
      // meter shows without anything having to count note-ons behind the engine's back.
      const uint16_t length = sequencer_.trackLength(t);
      uint8_t hits = 0;
      for (uint8_t back = 0; back < 4 && length > 0; ++back) {
        const uint16_t step =
            static_cast<uint16_t>((sequencer_.playhead(t) + length - back) % length);
        if (sequencer_.stepActive(t, step)) hits = static_cast<uint8_t>(hits | (1u << back));
      }
      line.activity = line.muted ? 0 : hits;
    }
  }

  frame.legend = modes_[mode_]->legend(state_);
  frame.contextTrack = static_cast<uint16_t>(state_.track + 1);
  frame.contextPattern = static_cast<uint16_t>(sequencer_.selectedPattern(state_.track) + 1);
  // Which 32 steps the grid is showing, for the modes that show steps at all.
  frame.contextFirstStep = 0;
  frame.contextLastStep = 0;
  uint8_t displayKind = 0;
  if (pageRowPadKind(displayKind) && displayKind == kPageSteps) {
    const uint16_t first = static_cast<uint16_t>(state_.stepPage * kStepsPerPage);
    frame.contextFirstStep = static_cast<uint16_t>(first + 1);
    frame.contextLastStep = static_cast<uint16_t>(first + kStepsPerPage);
  }
  // Every value starts as "not a voice", so a mode asks for a name by naming a port and the
  // rest need say nothing. One place sets the default, rather than every mode remembering to.
  for (uint8_t i = 0; i < kMaxDisplayValues; ++i) frame.values[i].voicePort = kNoDisplayPort;
  frame.numValues = frame.showTracks ? 0 : modes_[mode_]->displayValues(state_, frame.values);
}

// Whether a page holds anything, which is what the B row shows while a modifier is held.
bool UiController::hasPageData(uint8_t kind, uint8_t page) const {
  switch (kind) {
    case kPageSteps: {
      const uint16_t first = static_cast<uint16_t>(page * kStepsPerPage);
      const uint16_t length = sequencer_.trackLength(state_.track);
      for (uint16_t step = first; step < first + kStepsPerPage && step < length; ++step) {
        if (sequencer_.stepActive(state_.track, step)) return true;
      }
      return false;
    }
    case kPageProjects:
      return library_.projectPageHasData(page);
    case kPagePresets:
      return false;  // a preset is a number for the gear; no page of them holds anything
    default:
      // A track page lights for work of yours, so it looks past the built-in bank: every
      // track has that, and a row of eight pages all claiming to hold something would say
      // nothing at all.
      return anyPatternData(sequencer_, page * kTracksPerPage, kTracksPerPage, 0,
                            kFirstFactoryPattern);
  }
}

uint8_t UiController::pageKind() const {
  switch (mode_) {
    case kModeNote:
    case kModeStepParams:
    case kModeProbability:
      return kPageSteps;
    case kModeProject:
      return kPageProjects;
    case kModePreset:
      return kPagePresets;
    default:
      return kPageTracks;
  }
}

void UiController::selectPage(uint8_t button) {
  uint8_t kind = 0;
  uint8_t page = 0;
  if (!pageRowEntry(button, kind, page)) return;
  applyPage(kind, page);
}

void UiController::applyPage(uint8_t kind, uint8_t page) {
  switch (kind) {
    case kPageSteps:
      selectStepPage(page);
      break;
    case kPageProjects:
      state_.projectPage = page;
      break;
    case kPagePresets:
      state_.presetPage = page;
      break;
    default:
      selectTrackPage(page);
      break;
  }
}

void UiController::stepTrackPage(int8_t delta) {
  const int16_t page = static_cast<int16_t>(state_.trackPage) + delta;
  if (page < 0 || page >= kNumTrackPages) return;
  selectTrackPage(static_cast<uint8_t>(page));
}

// Flips the track page and keeps the selected track's position on it: track 4 -> 12.
void UiController::selectTrackPage(uint8_t page) {
  if (page == state_.trackPage) return;
  uint16_t track = page * kTracksPerPage + state_.track % kTracksPerPage;
  if (track >= kNumTracks) track = kNumTracks - 1;  // the last page can be short
  modes_[mode_]->reset();
  state_.trackPage = page;
  state_.track = static_cast<uint8_t>(track);
  clampStepPage();
  // A note mode Duplicate source is a step of the old track; pattern slots stay valid.
  if (mode_ == kModeNote) state_.duplicateSource = kNoSlot;
}

// Shows another page of steps. Held pads are dropped so they don't edit the new page.
void UiController::selectStepPage(uint8_t page) {
  if (mode_ == kModeNote && sequencer_.trackPianoRoll(state_.track)) {
    noteMode_.showRollStepPage(state_, page);  // the roll shows the page from its first step
    state_.duplicateSource = kNoSlot;
    return;
  }
  if (page == state_.stepPage) return;
  modes_[mode_]->reset();
  state_.stepPage = page;
  state_.duplicateSource = kNoSlot;
}

// Shift turns the top pad row into the pages of whatever the grid is showing - the step pages
// in note mode, step parameters and probability. One press goes straight to a page, and the
// same eight pads show which one you are on, so there is nothing to count and nothing to hold
// open. The piano roll pages differently and is left alone - every pad there is a note, so it
// has no row to spare. Pattern mode needs none of this: all 64 of a track's patterns are on
// the grid at once, so there is no page to pick.
bool UiController::pageRowPadKind(uint8_t& kind) const {
  if (mode_ == kModeNote && sequencer_.trackPianoRoll(state_.track)) return false;
  if (mode_ == kModeNote || mode_ == kModeStepParams || mode_ == kModeProbability) {
    kind = kPageSteps;
    return true;
  }
  return false;
}

// One page of the row. The step pages keep their playhead blink; the rest read as page
// buttons do, so a pad means the same thing here as the same page does on B1..B8.
Rgb UiController::pageRowPadColor(uint8_t kind, uint8_t page, uint32_t nowMs) const {
  if (kind == kPageSteps) return stepPageColor(page, nowMs);
  return pageColor(page, shownPage(kind), pageCount(kind), hasPageData(kind, page));
}

bool UiController::takePageRowPad(uint8_t pad, bool pressed) {
  if (pad >= kGridCols) return false;  // the top row only
  const uint8_t bit = static_cast<uint8_t>(1u << pad);
  if (!pressed) {
    // Only the release of a press this row took. Shift can be pressed in the middle of a
    // pad's tap, and then the release belongs to that pad - swallowing it would leave the
    // step off, or the pattern unlaunched, and the pad stuck down. Equally, Shift can be let
    // go before the pad, and that release is still the row's.
    if (!(pageRowHeld_ & bit)) return false;
    pageRowHeld_ = static_cast<uint8_t>(pageRowHeld_ & ~bit);
    return true;
  }
  uint8_t kind = 0;
  if (!state_.shiftHeld || !pageRowPadKind(kind)) return false;
  pageRowHeld_ = static_cast<uint8_t>(pageRowHeld_ | bit);
  if (pad < pageCount(kind)) applyPage(kind, pad);
  return true;
}

void UiController::clampStepPage() {
  if (state_.stepPage * kStepsPerPage >= sequencer_.trackLength(state_.track)) {
    state_.stepPage = 0;
  }
}

// Project mode pages on Shift + B1..B8 alone: no R button opens project mode any more, so
// there is none to hold there. Preset mode keeps its held R2 as
// well, because R2 is still the button preset mode is reached from - a tap there returns to
// the step parameters and a hold pages, which is the split paramsTapPending_ exists for.
bool UiController::pageSelectActive() const {
  return mode_ == kModeProject ? state_.shiftHeld
                               : mode_ == kModePreset &&
                                     (paramsButtonHeld_ || state_.shiftHeld);
}

bool UiController::trackPagingActive() const {
  // Every mode whose pads work on the selected track: the other track pages have to be
  // reachable without leaving, or a track past the eighth means going back to note mode,
  // paging, and returning. Without this the gesture fell through to picking a track, which
  // also threw you into note mode - the mode you were editing in, gone on a keypress.
  //
  // Not here: project and preset. Shift + B already picks their own page.
  if (!state_.shiftHeld) return false;
  switch (mode_) {
    case kModeNote:
    case kModePattern:
    case kModeScale:
    case kModeStepParams:
    case kModeProbability:
    case kModeScene:
    case kModeGlobal:
      return true;
    default:
      return false;
  }
}

// The piano roll still pages by holding R1, because its grid has no step area with corners
// to put the gesture on: Shift there ends the pattern, and every pad is a note.
bool UiController::stepPagingActive() const {
  return mode_ == kModeNote && noteButtonHeld_ && sequencer_.trackPianoRoll(state_.track);
}

void UiController::selectMode(uint8_t mode) {
  if (mode == mode_) return;
  modes_[mode_]->reset();
  mode_ = mode;
  state_.duplicateSource = kNoSlot;
  // Project and preset mode open on the page holding what is currently selected.
  if (mode == kModeProject) state_.projectPage = pageOf(library_.currentProject());
  if (mode == kModePreset) {
    // Open on the track's own voice, window and page both - so a track on a piano and one on
    // a kit each come up where their sound is, with no window to remember or set.
    const uint16_t preset = sequencer_.trackPreset(state_.track);
    state_.presetWindow = PresetMode::windowOf(preset);
    state_.presetPage = PresetMode::pageInWindow(preset);
  }
}

// Track buttons return to note mode with that track selected.
void UiController::selectTrack(uint8_t track) {
  // A mode that shows the selected track's own content keeps you in it: picking another
  // track while setting velocities means you want that track's velocities, not its steps,
  // and in pattern mode B1..B8 is how you walk the tracks, exactly as in note mode.
  // Project and preset mode go to note mode with the track, which is how a track is picked
  // there.
  const bool stay = mode_ == kModePattern || mode_ == kModeGlobal || mode_ == kModeScale ||
                    mode_ == kModeStepParams || mode_ == kModeProbability;
  if (track == state_.track && (stay || mode_ == kModeNote)) return;
  modes_[mode_]->reset();
  if (!stay) mode_ = kModeNote;
  state_.track = track;
  clampStepPage();
  // A Duplicate source of the old track's steps is meaningless on the new one; a pattern slot
  // names its own track, so it stays valid and a pattern can be copied to another track.
  if (mode_ != kModePattern) state_.duplicateSource = kNoSlot;
}

void UiController::render(LedFrame& frame, uint32_t nowMs) const {
  renderFunctionButtons(frame);

  if (pageSelectActive()) {
    renderLibraryPageButtons(frame);
  } else if (stepPagingActive()) {
    renderStepPageButtons(frame, nowMs);
  } else if (trackPagingActive()) {
    renderTrackPageButtons(frame);
  } else {
    renderTrackButtons(frame);
  }
  frame.shift = state_.shiftHeld ? kWhite : dim(kWhite, kIdleLevel);
  renderMixButtons(frame);
  renderPagePanel(frame, nowMs);

  modes_[mode_]->renderPads(state_, frame);

  // Last, over whatever the mode drew: Shift turns the top row into this mode's pages. It
  // picks one outright and shows which you are on in the same eight pads, so there is nothing
  // to hold open and nothing to count. Asked of the same predicate the gesture uses, so a lit
  // pad always does something and a dark one never does.
  uint8_t rowKind = 0;
  if (state_.shiftHeld && pageRowPadKind(rowKind)) {
    for (uint8_t page = 0; page < kGridCols; ++page) {
      frame.pads[page] = pageRowPadColor(rowKind, page, nowMs);
    }
  }
}

uint8_t UiController::ccTrack(uint8_t group, uint8_t index) const {
  if (group == kGroupMasterFader || group == kGroupMixMaster) return state_.track;
  const uint8_t strip = group == kGroupKnob ? static_cast<uint8_t>(index % kNumMixStrips)
                                            : index;
  const uint16_t track = state_.trackPage * kTracksPerPage + strip;
  return track < kNumTracks ? static_cast<uint8_t>(track) : state_.track;
}

void UiController::sendAllControls() {
  static const uint8_t kGroups[] = {kGroupFader, kGroupMasterFader, kGroupMixFader,
                                    kGroupMixMaster, kGroupKnob};
  static const uint8_t kCounts[] = {kNumTrackFaders, 1, kNumMixStrips, 1, kNumKnobs};
  for (uint8_t g = 0; g < sizeof(kGroups); ++g) {
    for (uint8_t index = 0; index < kCounts[g]; ++index) {
      const uint8_t slot = ControlMap::slotIndex(kGroups[g], index);
      if (slot == kNoCcSlot || lastCc_[slot] == kNoCc) continue;  // never heard from
      const CcAssignment& assignment = controls_.assignment(kGroups[g], index);
      if (assignment.cc == kNoCc) continue;
      sequencer_.sendControlChange(ccTrack(kGroups[g], index), assignment.cc, lastCc_[slot]);
    }
  }
}

void UiController::sendControlCc(uint8_t group, uint8_t index, uint16_t position, bool initial) {
  const uint8_t slot = ControlMap::slotIndex(group, index);
  if (slot == kNoCcSlot) return;
  const CcAssignment& assignment = controls_.assignment(group, index);
  if (assignment.cc == kNoCc) return;
  const uint8_t value = ControlMap::ccValue(assignment, position);
  if (lastCc_[slot] == value) return;
  lastCc_[slot] = value;
  // A start-up scan says where the control already sits; only a move is played.
  if (initial) return;
  sequencer_.sendControlChange(ccTrack(group, index), assignment.cc, value);
}

// The mixer's A1 row mutes the strip's track; with the mixer's Shift (M4) it solos it.
void UiController::handleMuteButton(uint8_t strip) {
  const uint16_t track = state_.trackPage * kTracksPerPage + strip;
  if (track >= kNumTracks) return;
  const uint8_t t = static_cast<uint8_t>(track);
  if ((mixSideHeld_ >> kMixShiftButton) & 1) {
    sequencer_.setTrackSoloed(t, !sequencer_.trackSoloed(t));
  } else {
    sequencer_.setTrackMuted(t, !sequencer_.trackMuted(t));
  }
}

// A mute button shows whether its track is heard: white when soloed, the track colour when
// it plays, half-lit when another track's solo is silencing it, and darkest when it is muted,
// so a mute can still be told from a solo elsewhere. A2 has no function and only lights while
// it is held - dim enough that the MIDI Mix's on-or-off LED stays dark until it is pressed.
void UiController::renderMixButtons(LedFrame& frame) const {
  for (uint8_t strip = 0; strip < kNumMixStrips; ++strip) {
    const uint16_t index = state_.trackPage * kTracksPerPage + strip;
    const bool present = index < kNumTracks;
    const uint8_t track = present ? static_cast<uint8_t>(index) : 0;
    const Rgb color = present ? trackColor(track) : kWhite;

    // The mute button lights when its track is NOT sounding, the way a mute button reads on a
    // mixer: dark while you hear the track, lit when it is muted or another track's solo has
    // taken it out. Soloing one track therefore lights every button but that one.
    Rgb mute = kBlack;
    if (present) {
      if (sequencer_.trackAudible(track)) {
        // A faint light so the strip still shows its track colour, and its own solo in white.
        mute = dim(sequencer_.trackSoloed(track) ? kWhite : color, kIdleLevel);
      } else {
        mute = sequencer_.trackMuted(track) ? color : dim(color, kSilencedLevel);
      }
    }
    frame.mixButtons[mixButtonIndex(0, strip)] = mute;

    const uint8_t a2 = mixButtonIndex(1, strip);
    const bool a2Held = (mixButtonsHeld_ >> a2) & 1;
    frame.mixButtons[a2] = a2Held ? kWhite : dim(kWhite, kIdleLevel);
  }
  for (uint8_t button = 0; button < kNumMixSideButtons; ++button) {
    const bool held = (mixSideHeld_ >> button) & 1;
    frame.mixSide[button] = held ? kWhite : dim(kWhite, kIdleLevel);
  }
}

void UiController::renderFunctionButtons(LedFrame& frame) const {
  for (uint8_t button = 0; button < kNumRightButtons; ++button) {
    bool lit = false;
    switch (button) {
      case kButtonPattern:
        lit = mode_ == kModePattern || mode_ == kModeScene;  // Shift + R4's mode
        break;
      case kButtonClear:
        lit = state_.clearHeld || mode_ == kModeProject;  // Shift + R5's mode
        break;
      case kButtonDuplicate:
        lit = state_.duplicateHeld;
        break;
      case kButtonRecord:
        lit = sequencer_.recording();
        break;
      case kButtonPlay:
        lit = sequencer_.playing();
        break;
      default:
        // Every mode lights the button it is reached from, the ones on a Shift layer
        // included: scale sits on note, preset on the step parameters, the settings on
        // probability's R3. Spelled out per mode rather than comparing the button to the mode -
        // those were once the same number, and that held the two enums together by accident.
        lit = (button == kButtonNote && (mode_ == kModeNote || mode_ == kModeScale)) ||
              (button == kButtonParams &&
               (mode_ == kModeStepParams || mode_ == kModePreset)) ||
              (button == kButtonProbability &&
               (mode_ == kModeProbability || mode_ == kModeGlobal));
        break;
    }
    frame.right[button] = lit ? kButtonColors[button] : dim(kButtonColors[button], kIdleLevel);
  }
}

void UiController::renderTrackButtons(LedFrame& frame) const {
  for (uint8_t button = 0; button < kNumBottomButtons; ++button) {
    const uint16_t track = state_.trackPage * kTracksPerPage + button;
    if (track >= kNumTracks) {
      frame.bottom[button] = kBlack;
      continue;
    }
    const Rgb color = trackColor(static_cast<uint8_t>(track));
    frame.bottom[button] = (track == state_.track) ? color : dim(color, kIdleLevel);
  }
}

// Shift held, or R2 in preset mode: B1..B8 show project or preset pages.
void UiController::renderLibraryPageButtons(LedFrame& frame) const {
  const bool projects = (mode_ == kModeProject);
  const uint8_t shown = projects ? state_.projectPage : state_.presetPage;
  for (uint8_t page = 0; page < kNumBottomButtons; ++page) {
    const bool hasData = projects && library_.projectPageHasData(page);
    frame.bottom[page] = pageColor(page, shown, kNumPages, hasData);
  }
}

void UiController::renderTrackPageButtons(LedFrame& frame) const {
  for (uint8_t page = 0; page < kNumBottomButtons; ++page) {
    const bool hasData = anyPatternData(sequencer_, page * kTracksPerPage, kTracksPerPage, 0,
                                        kFirstFactoryPattern);  // your work, not the bank
    frame.bottom[page] = pageColor(page, state_.trackPage, kNumTrackPages, hasData);
  }
}

// R1 held in note mode: B1..B8 show step pages, and the page being played blinks.
// What one step page looks like: green the page on screen, dim blue one holding steps, grey
// an empty one, black where this build has no such page, white while the playhead is on it.
// One rule, because the same map is drawn on the pads under Shift and on B1..B8 in the roll.
// A panel whose only job is pages: one row per kind, one pad per page, every kind visible at
// once rather than only the kind the open mode happens to page over. The pads read exactly as
// the top row's do under Shift - the same function answers both - so a page means the same
// thing wherever it is shown, and the step row keeps its playhead blink.
void UiController::renderPagePanel(LedFrame& frame, uint32_t nowMs) const {
  for (uint8_t row = 0; row < kNumPageKinds; ++row) {
    const uint8_t kind = kPanelRowKind[row];
    for (uint8_t page = 0; page < kPagesPerKind; ++page) {
      frame.pages[pagePadIndex(row, page)] = pageRowPadColor(kind, page, nowMs);
    }
  }
  // The window row reads as the windows do inside preset mode: the one on screen, one that
  // reaches voices, and one past the end of the device's list. It sits straight above the
  // preset page row, so the two are a tab strip and its pages - pick a window on one, a page
  // of it on the other.
  for (uint8_t window = 0; window < kNumPanelWindows; ++window) {
    frame.pageWindows[window] =
        window == state_.presetWindow      ? kSelectedColor
        : presetMode_.windowHasVoices(state_, window) ? kFilledColor
                                                      : kEmptyColor;
  }
  // The mode buttons: the one whose mode is open, and the rest waiting.
  for (uint8_t button = 0; button < kNumPanelModes; ++button) {
    frame.panelModes[button] = mode_ == kPanelModeOf[button] ? kSelectedColor : kEmptyColor;
  }
  // The arrows, lit only where there is somewhere to go - and dark altogether when no piano
  // roll is on screen, since that is the only thing they move.
  const bool roll = rollArrowsActive();
  for (uint8_t arrow = 0; arrow < kNumPanelArrows; ++arrow) {
    frame.panelArrows[arrow] =
        roll && noteMode_.canScrollRoll(state_, arrow) ? kSelectedColor : kBlack;
  }
}

// Whether the arrow buttons have anything to move: note mode, showing a track whose grid is a
// piano roll. Anywhere else they are dark and inert rather than guessing at a meaning.
bool UiController::rollArrowsActive() const {
  return mode_ == kModeNote && sequencer_.trackPianoRoll(state_.track);
}

// An arrow nudges the piano roll by kRollScrollBy - one step, or one note of the track's
// scale. Exactly what the roll's own pad cluster moves, through the same call, so the only
// difference between the two is that these need no Shift.
void UiController::handlePanelArrow(uint8_t arrow) {
  if (arrow >= kNumPanelArrows || !rollArrowsActive()) return;
  noteMode_.scrollRoll(state_, arrow);
}

// A panel mode button opens its mode outright, with no modifier - which is the whole point of
// having them: the five modes that need Shift on the APC are one press away here.
void UiController::handlePanelMode(uint8_t button) {
  if (button >= kNumPanelModes) return;
  selectMode(kPanelModeOf[button]);
}

// A page pad switches to that page of that kind outright. It needs no modifier and no mode:
// the panel is not part of the grid, so there is nothing for it to be confused with.
void UiController::handlePagePad(uint8_t pad) {
  if (pad >= kNumPagePads) return;
  const uint8_t kind = kPanelRowKind[pad / kPagesPerKind];
  const uint8_t page = static_cast<uint8_t>(pad % kPagesPerKind);
  // applyPage trusts its caller, as it does for the page row: each checks the page exists
  // before asking for it. Every kind fills its row in a full build, so
  // this only bites one configured smaller - GX_NUM_TRACKS=32 leaves four track pages.
  if (page >= pageCount(kind)) return;
  applyPage(kind, page);
}

// The panel's window row moves preset mode's 512-voice window. A window the device has no
// voices for is refused, exactly as Shift + the bottom pad row refuses it inside the mode.
void UiController::handlePresetWindow(uint8_t window) {
  if (window >= kNumPanelWindows) return;
  if (!presetMode_.windowHasVoices(state_, window)) return;
  state_.presetWindow = window;
}

Rgb UiController::stepPageColor(uint8_t page, uint32_t nowMs) const {
  if (page >= kNumStepPages) return kBlack;
  const uint8_t track = state_.track;
  const uint16_t playingPage = sequencer_.playhead(track) / kStepsPerPage;
  if (sequencer_.playing() && page == playingPage && blinkOn(nowMs)) return kWhite;
  if (page == state_.stepPage) return kSelectedColor;
  const uint16_t length = sequencer_.trackLength(track);
  const uint16_t first = static_cast<uint16_t>(page * kStepsPerPage);
  for (uint16_t step = first; step < first + kStepsPerPage && step < length; ++step) {
    if (sequencer_.stepActive(track, step)) return kFilledColor;
  }
  return kEmptyColor;
}

void UiController::renderStepPageButtons(LedFrame& frame, uint32_t nowMs) const {
  for (uint8_t page = 0; page < kNumBottomButtons; ++page) {
    frame.bottom[page] = stepPageColor(page, nowMs);
  }
}

const char* UiController::modeName(uint8_t mode) {
  return mode < kNumModes ? kModeNames[mode] : "";
}

const char* UiController::buttonName(uint8_t button) {
  return button < kNumRightButtons ? kButtonNames[button] : "";
}

}  // namespace gx
