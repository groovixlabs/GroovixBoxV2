#include "ui/ArrangementMode.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kFirstSongPad = kNumScenes;  // the scenes fill the top half
const uint8_t kEmptyStepLevel = 14;        // a song step with nothing in it
const uint8_t kHeldSceneLevel = 255;       // the scene of the step being held

// Scenes take the eight track hues; the later banks of eight are the same hues, dimmer. Songs
// rarely use more than a handful of scenes, so in practice each one reads as its own colour.
const uint8_t kBankLevels[4] = {255, 150, 95, 60};
const uint8_t kIdlePositionLevel = 90;  // the position marker when the song isn't running

Rgb sceneColor(uint8_t scene) {
  return dim(trackColor(scene % kGridCols), kBankLevels[(scene / kGridCols) & 3]);
}

const char* const kSceneLabels[kNumScenes] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32"};
// A song step says which scene it plays.
const char* const kSongLabels[kNumScenes] = {
    "S1",  "S2",  "S3",  "S4",  "S5",  "S6",  "S7",  "S8",  "S9",  "S10", "S11",
    "S12", "S13", "S14", "S15", "S16", "S17", "S18", "S19", "S20", "S21", "S22",
    "S23", "S24", "S25", "S26", "S27", "S28", "S29", "S30", "S31", "S32"};

static_assert(kNumScenes + kNumSongSteps <= kNumPads, "the scenes and the song share the grid");

}  // namespace

ArrangementMode::ArrangementMode(Sequencer& sequencer)
    : sequencer_(sequencer), heldStep_(kNoPad), assigned_(false) {}

void ArrangementMode::reset() {
  heldStep_ = kNoPad;
  assigned_ = false;
}

void ArrangementMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (pad >= kFirstSongPad) {
    const uint8_t step = static_cast<uint8_t>(pad - kFirstSongPad);
    if (step >= kNumSongSteps) return;
    if (!pressed) {
      // A step let go without choosing a scene was a tap: go there and hear it.
      if (heldStep_ == step) {
        if (!assigned_) sequencer_.goToSongStep(step);
        heldStep_ = kNoPad;
        assigned_ = false;
      }
      return;
    }
    if (state.clearHeld) {
      sequencer_.clearSongStep(step);
      return;
    }
    if (state.duplicateHeld) {
      uint16_t from = kNoSlot;
      if (takeDuplicateTarget(state, step, from)) {
        sequencer_.copySongStep(static_cast<uint8_t>(from), step);
      }
      return;
    }
    heldStep_ = step;
    assigned_ = false;
    return;
  }

  if (!pressed || pad >= kNumScenes) return;
  // A scene tapped while a step is held goes into that step; on its own it just plays.
  if (heldStep_ != kNoPad) {
    sequencer_.setSongStep(heldStep_, pad);
    assigned_ = true;
    return;
  }
  sequencer_.queueScene(pad);
}

void ArrangementMode::renderPads(const UiState& state, LedFrame& frame) const {
  for (uint8_t pad = 0; pad < kNumPads; ++pad) frame.pads[pad] = kBlack;

  const uint8_t heldScene = heldStep_ == kNoPad ? kNoScene : sequencer_.songStep(heldStep_);
  for (uint8_t scene = 0; scene < kNumScenes; ++scene) {
    const bool used = sequencer_.sceneUsed(scene);
    Rgb color;
    if (scene == heldScene) {
      color = dim(sceneColor(scene), kHeldSceneLevel);  // what the held step plays
    } else if (scene == sequencer_.pendingScene()) {
      color = blinkOn(state.nowMs) ? kSelectedColor : kFilledColor;
    } else if (scene == sequencer_.currentScene()) {
      color = kSelectedColor;
    } else {
      color = used ? kFilledColor : kEmptyColor;
    }
    frame.pads[scene] = color;
  }

  for (uint8_t step = 0; step < kNumSongSteps; ++step) {
    const uint8_t scene = sequencer_.songStep(step);
    Rgb color;
    if (scene == kNoScene) {
      color = dim(kWhite, kEmptyStepLevel);
    } else if (step == sequencer_.songPosition()) {
      // Bright while the song is playing itself, dim when it is only where you left off.
      color = sequencer_.followingSong() ? kWhite : dim(kWhite, kIdlePositionLevel);
    } else {
      color = sceneColor(scene);
    }
    if (state.duplicateHeld && state.duplicateSource == step) color = kCopySourceColor;
    frame.pads[kFirstSongPad + step] = color;
  }
}

const char* ArrangementMode::padLabel(const UiState&, uint8_t pad) const {
  if (pad < kNumScenes) return kSceneLabels[pad];
  const uint8_t step = static_cast<uint8_t>(pad - kFirstSongPad);
  if (step >= kNumSongSteps) return nullptr;
  const uint8_t scene = sequencer_.songStep(step);
  return scene == kNoScene ? nullptr : kSongLabels[scene];
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kSceneColor = {30, 156, 255};
const Rgb kStepColor = {160, 70, 255};
const Rgb kHoldColor = {243, 244, 246};

const ModeLegend kSongLegend = {
    "SHIFT + R8",
    "Song",
    {
        {1, 4, kSceneColor, "rows 1-4", "the 32 scenes to build from"},
        {5, 8, kStepColor, "rows 5-8", "the 32 song steps, four bars each"},
        {0, 0, kHoldColor, "hold step", "then a scene, to put it in that step"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"R8 here plays the song from the marker", "tap a step goes there at once",
     "R5 + step empties it, R6 + step + step repeats a section", NULL},
    3,
    {kSceneColor, kSceneColor, kSceneColor, kSceneColor,
     kStepColor, kStepColor, kStepColor, kStepColor},
};

}  // namespace

const ModeLegend* ArrangementMode::legend(const UiState&) const { return &kSongLegend; }

}  // namespace gx
