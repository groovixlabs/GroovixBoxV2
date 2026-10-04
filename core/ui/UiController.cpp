#include "ui/UiController.h"

#include "engine/FactoryPatterns.h"
#include "ui/Palette.h"

namespace gx {

static_assert(kButtonPlay < kNumRightButtons, "every function needs a button");
static_assert(kNumBottomButtons == kTracksPerPage, "track buttons sit under the pad columns");
static_assert(kNumTrackPages <= kNumBottomButtons,
              "the track pages have to fit on B1..B8");
static_assert(kNumPatternPages <= kGridCols,
              "the pattern pages have to fit the top pad row under Shift");
static_assert(kNumStepPages <= kGridCols,
              "and so do the step pages");

namespace {

const char* const kModeNames[kNumModes] = {"PROJECT", "PATTERN", "NOTE",  "PRESET", "SCALE",
                                           "PARAMS",  "GLOBAL",  "PROB",  "SCENE",  "SONG"};

const char* const kButtonNames[kNumRightButtons] = {
    "PROJECT", "PATTERN", "NOTE", "PARAMS", "CLEAR", "DUPLICATE", "RECORD", "PLAY",
};

const Rgb kButtonColors[kNumRightButtons] = {
    {255, 255, 255},  // R1 project
    {255, 255, 255},  // R2 pattern
    {255, 255, 255},  // R3 note
    {255, 255, 255},  // R4 preset
    {255, 90, 0},     // R5 clear
    {0, 150, 255},    // R6 duplicate
    {255, 0, 0},      // R7 record
    {0, 255, 0},      // R8 play
};

// Labels for Shift + a button, by what it does in the current mode.
// R6 and R7 are blank because the APC's firmware keeps them for its Drum and Note modes.
const char* const kShiftRightLabels[kNumRightButtons] = {"GLOBAL", "SCENE", "SCALE", "PRESET",
                                                         "PROB",   "",      "",      "SONG"};
const char* const kPatternPageLabels[kNumBottomButtons] = {
    "PAT 1-8", "PAT 9-16", "PAT 17-24", "PAT 25-32", "PAT 33-40", "PAT 41-48", "PAT 49-56", "PAT 57-64"};
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

const uint8_t kIdleLevel = 40;         // buttons that are not active
const uint8_t kSilencedLevel = 130;    // a mixer strip another track's solo has silenced
const uint8_t kPageDataLevel = 90;     // a page with something on it, on the mixer's A2 row
const uint8_t kPageEmptyLevel = 30;    // and one without

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
      projectButtonHeld_(false),
      paramsButtonHeld_(false),
      noteButtonHeld_(false),
      paramsTapPending_(false),
      recordTapPending_(false),
      mixShiftTapPending_(false) {
  state_.track = 0;
  state_.stepPage = 0;
  state_.trackPage = 0;
  state_.patternPage = 0;
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
        if (event.pressed && event.index < kNumMixStrips) {
          handleMuteButton(event.index);
        } else if (event.pressed) {
          // The A2 row (REC ARM on a MIDI Mix) picks the pages of the open mode, so paging
          // never needs Shift + a track button, which can select a track by mistake.
          selectPage(static_cast<uint8_t>(event.index - kNumMixStrips));
        }
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
  if (patternModePaging()) return button < kNumTrackPages ? kTrackPageLabels[button] : "";
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
    if (pad >= pageCount(kind)) return "";
    return kind == kPageSteps ? kStepPageLabels[pad] : kPatternPageLabels[pad];
  }
  return modes_[mode_]->padLabel(state_, pad);
}

void UiController::handleButton(uint8_t button, bool pressed) {
  // Shift + R3 opens scale mode everywhere else; R3 alone returns to note mode.
  if (pressed && button == kButtonNote && state_.shiftHeld) {
    selectMode(kModeScale);
    return;
  }
  // Shift + R4 opens preset mode; R4 on its own is the step parameters, which are edited far
  // more often than a patch is picked.
  if (pressed && button == kButtonParams && state_.shiftHeld) {
    selectMode(kModePreset);
    return;
  }
  // Shift + R1 opens global settings; R1 alone goes to project mode as usual.
  if (pressed && button == kButtonProject && state_.shiftHeld) {
    selectMode(kModeGlobal);
    return;
  }
  // Shift + R2 opens scene mode; R2 alone goes to pattern mode as usual.
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
  // Shift + R5 opens probability mode; R5 on its own stays the Clear modifier.
  if (pressed && button == kButtonClear && state_.shiftHeld) {
    selectMode(kModeProbability);
    return;
  }
  // In preset mode R4 is held to pick a preset page with B1..B8, so it acts on release: a tap
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
    case kButtonProject:
      projectButtonHeld_ = pressed;
      if (pressed) selectMode(kModeProject);
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

// B1..B8 normally return to note mode on a track of the current track page; in global
// settings they change the track without leaving the mode, so a channel, port or instrument
// can be given to one track after another. In pattern mode they page the grid instead:
// B1..B4 the track pages, B5..B8 the pattern pages, whether or not Shift is held. While
// R1/R4 is held, or Shift in project/preset mode, they pick a project/preset page, and with
// Shift in note or global settings mode a track page. Changing page keeps a picked Duplicate source, so items
// can be copied between pages.
void UiController::handleTrackButton(uint8_t button) {
  if (pageSelectActive()) {
    if (mode_ == kModeProject) {
      state_.projectPage = button;
    } else {
      paramsTapPending_ = false;  // R4 was used for a page, so its release stays in the mode
      state_.presetPage = button;
    }
  } else if (stepPagingActive()) {
    if (button < kNumStepPages) selectStepPage(button);
  } else if (trackPagingActive()) {
    if (button < kNumTrackPages) selectTrackPage(button);
  } else if (patternModePaging()) {
    // Pattern mode's bottom row moves the window over the grid: B1..B4 the columns it shows
    // (track pages), B5..B8 the rows (pattern pages) — the row the mixer's A2 buttons mirror.
    // No modifier, so Shift + R2 means scenes here as it does everywhere else, Shift held for
    // the mutes still leaves the pages reachable, and a track is picked by touching a column.
    selectPage(button);
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
    case kPagePatterns:
      return kNumPatternPages;
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
    case kPagePatterns:
      return state_.patternPage;
    case kPageProjects:
      return state_.projectPage;
    case kPagePresets:
      return state_.presetPage;
    default:
      return state_.trackPage;
  }
}

// What button 0..7 of a page row picks. One row, two surfaces: the APC's B1..B8 in pattern
// mode and the mixer's A2 row everywhere, so both show and pick the same pages. One mapping
// for every mode - pattern mode used to split the row, half tracks and half patterns, and
// its pattern pages are on the top pad row now, where the page you are on is visible.
// False for a button this build has no page for.
bool UiController::pageRowEntry(uint8_t button, uint8_t& kind, uint8_t& page) const {
  kind = pageKind();
  page = button;
  return page < pageCount(kind);
}

// The mixer's A2 row shows the pages of the open mode, the same ones the APC's bottom row
// shows in pattern mode. The panel's LEDs are on or off, so only the page you are on is
// bright enough to light there; on screen the others still show which pages hold something.
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
  frame.numValues = frame.showTracks ? 0 : modes_[mode_]->displayValues(state_, frame.values);
}

void UiController::renderMixPageButtons(LedFrame& frame, uint32_t nowMs) const {
  // The page the playhead is on blinks white, as it used to on the track buttons while R3
  // was held. Here it needs nothing held, so where the music is is always visible.
  const uint16_t playingPage = sequencer_.playhead(state_.track) / kStepsPerPage;
  const bool blink = sequencer_.playing() && blinkOn(nowMs);
  for (uint8_t button = 0; button < kNumMixStrips; ++button) {
    const uint8_t index = mixButtonIndex(1, button);
    uint8_t kind = 0;
    uint8_t page = 0;
    if (!pageRowEntry(button, kind, page)) {
      frame.mixButtons[index] = kBlack;  // this build has no such page
      continue;
    }
    if (kind == kPageSteps && blink && page == playingPage) {
      frame.mixButtons[index] = kWhite;
      continue;
    }
    if (page == shownPage(kind)) {
      frame.mixButtons[index] = kSelectedColor;
      continue;
    }
    frame.mixButtons[index] = dim(kFilledColor, hasPageData(kind, page) ? kPageDataLevel
                                                                        : kPageEmptyLevel);
  }
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
    case kPagePatterns:
      return anyPatternData(sequencer_, state_.trackPage * kTracksPerPage, kTracksPerPage,
                            page * kPatternsPerPage, kPatternsPerPage);
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
    case kPagePatterns:
      state_.patternPage = page;
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

// Shift turns the top pad row into the pages of whatever the grid is showing: the step pages
// in note mode, step parameters and probability, the pattern pages in pattern mode. One press
// goes straight to a page, and the same eight pads show which one you are on, so there is
// nothing to count and nothing to hold open. The piano roll pages differently and is left
// alone - every pad there is a note, so it has no row to spare.
bool UiController::pageRowPadKind(uint8_t& kind) const {
  if (mode_ == kModeNote && sequencer_.trackPianoRoll(state_.track)) return false;
  if (mode_ == kModeNote || mode_ == kModeStepParams || mode_ == kModeProbability) {
    kind = kPageSteps;
    return true;
  }
  if (mode_ == kModePattern) {
    kind = kPagePatterns;
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

bool UiController::pageSelectActive() const {
  return (mode_ == kModeProject && (projectButtonHeld_ || state_.shiftHeld)) ||
         (mode_ == kModePreset && (paramsButtonHeld_ || state_.shiftHeld));
}

bool UiController::trackPagingActive() const {
  // Every mode whose pads work on the selected track: the other track pages have to be
  // reachable without leaving, or a track past the eighth means going back to note mode,
  // paging, and returning. Without this the gesture fell through to picking a track, which
  // also threw you into note mode - the mode you were editing in, gone on a keypress.
  //
  // Not here: project, preset and pattern. Shift + B already picks their own page, and
  // pattern mode's bottom row is the page row with or without Shift.
  if (!state_.shiftHeld) return false;
  switch (mode_) {
    case kModeNote:
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

// Pattern mode's bottom row is the page row however you are holding the panel. Shift there
// means one thing only - the bottom pad row is the mutes - so holding it to silence a track
// never takes the pages away, and the buttons never change under your hand.
bool UiController::patternModePaging() const { return mode_ == kModePattern; }

// The piano roll still pages by holding R3, because its grid has no step area with corners
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
  // track while setting velocities means you want that track's velocities, not its steps.
  // The slot modes go to note mode with the track, which is how a track is picked there.
  const bool stay = mode_ == kModeGlobal || mode_ == kModeScale ||
                    mode_ == kModeStepParams || mode_ == kModeProbability;
  if (track == state_.track && (stay || mode_ == kModeNote)) return;
  modes_[mode_]->reset();
  if (!stay) mode_ = kModeNote;
  state_.track = track;
  clampStepPage();
  state_.duplicateSource = kNoSlot;
}

void UiController::render(LedFrame& frame, uint32_t nowMs) const {
  renderFunctionButtons(frame);

  if (pageSelectActive()) {
    renderLibraryPageButtons(frame);
  } else if (stepPagingActive()) {
    renderStepPageButtons(frame, nowMs);
  } else if (trackPagingActive()) {
    renderTrackPageButtons(frame);
  } else if (patternModePaging()) {
    renderPatternModeButtons(frame);
  } else {
    renderTrackButtons(frame);
  }
  frame.shift = state_.shiftHeld ? kWhite : dim(kWhite, kIdleLevel);
  renderMixButtons(frame, nowMs);

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
// so a mute can still be told from a solo elsewhere. A2 has no function yet and only lights
// while it is held.
void UiController::renderMixButtons(LedFrame& frame, uint32_t nowMs) const {
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

  }
  renderMixPageButtons(frame, nowMs);
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
        lit = mode_ == kModePattern || mode_ == kModeScene;  // Shift + R2's mode
        break;
      case kButtonClear:
        lit = state_.clearHeld || mode_ == kModeProbability;  // Shift + R5's mode
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
        // Scale, step parameters and global settings light R3, R4 and R1: the buttons
        // their Shift layers belong to.
        lit = (button == mode_) || (mode_ == kModeScale && button == kButtonNote) ||
              (mode_ == kModeStepParams && button == kButtonParams) ||
              (mode_ == kModeGlobal && button == kButtonProject);
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

// R1/R4 held: B1..B8 show project or preset pages.
void UiController::renderLibraryPageButtons(LedFrame& frame) const {
  const bool projects = (mode_ == kModeProject);
  const uint8_t shown = projects ? state_.projectPage : state_.presetPage;
  for (uint8_t page = 0; page < kNumBottomButtons; ++page) {
    const bool hasData = projects && library_.projectPageHasData(page);
    frame.bottom[page] = pageColor(page, shown, kNumPages, hasData);
  }
}

// Pattern mode: B1..B4 show the track pages - which eight tracks are the grid's columns -
// lit for the page on screen, half lit for a page holding something. The pattern pages are on
// the top pad row under Shift. The mixer's A2 row shows the same, from the same mapping.
void UiController::renderPatternModeButtons(LedFrame& frame) const {
  for (uint8_t button = 0; button < kNumBottomButtons; ++button) {
    uint8_t kind = 0;
    uint8_t page = 0;
    if (!pageRowEntry(button, kind, page)) {
      frame.bottom[button] = kBlack;
      continue;
    }
    frame.bottom[button] = pageColor(page, shownPage(kind), pageCount(kind),
                                     hasPageData(kind, page));
  }
}


void UiController::renderTrackPageButtons(LedFrame& frame) const {
  for (uint8_t page = 0; page < kNumBottomButtons; ++page) {
    const bool hasData = anyPatternData(sequencer_, page * kTracksPerPage, kTracksPerPage, 0,
                                        kFirstFactoryPattern);  // your work, not the bank
    frame.bottom[page] = pageColor(page, state_.trackPage, kNumTrackPages, hasData);
  }
}

// R3 held in note mode: B1..B8 show step pages, and the page being played blinks.
// What one step page looks like: green the page on screen, dim blue one holding steps, grey
// an empty one, black where this build has no such page, white while the playhead is on it.
// One rule, because the same map is drawn on the pads under Shift and on B1..B8 in the roll.
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
