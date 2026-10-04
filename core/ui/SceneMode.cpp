#include "ui/SceneMode.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kFirstMutePad = (kGridRows - 1) * kGridCols;  // the bottom row
const uint8_t kMutedLevel = 20;        // a track this scene silences
const uint8_t kCaptureLevel = 60;      // scene pads while Record is held
const Rgb kCaptureColor = {255, 0, 0};

const char* const kSceneLabels[kNumScenes] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31", "32"};
const char* const kMuteLabels[kGridCols] = {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8"};

static_assert(kNumScenes <= kFirstMutePad, "the scene pads sit above the mute row");

}  // namespace

SceneMode::SceneMode(Sequencer& sequencer) : sequencer_(sequencer) {}

void SceneMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (!pressed) return;

  if (pad >= kFirstMutePad) {
    const uint16_t track = state.trackPage * kTracksPerPage + (pad - kFirstMutePad);
    if (track < kNumTracks) {
      const uint8_t t = static_cast<uint8_t>(track);
      sequencer_.setTrackMuted(t, !sequencer_.trackMuted(t));
    }
    return;
  }
  if (pad >= kNumScenes) return;

  if (state.clearHeld) {
    sequencer_.clearScene(pad);
    return;
  }
  if (state.duplicateHeld) {
    uint16_t from = kNoSlot;
    if (takeDuplicateTarget(state, pad, from)) sequencer_.copyScene(static_cast<uint8_t>(from), pad);
    return;
  }
  // Record is the capture modifier here: everything else launches.
  if (state.recordHeld) {
    sequencer_.captureScene(pad);
    return;
  }
  sequencer_.queueScene(pad);
}

void SceneMode::renderPads(const UiState& state, LedFrame& frame) const {
  for (uint8_t pad = 0; pad < kNumPads; ++pad) frame.pads[pad] = kBlack;

  // Scenes read like the project and preset slots: grey empty, blue holds something, green
  // playing. While Record is held they turn red, because tapping then writes.
  for (uint8_t scene = 0; scene < kNumScenes; ++scene) {
    const bool used = sequencer_.sceneUsed(scene);
    Rgb color;
    if (state.recordHeld) {
      color = used ? kCaptureColor : dim(kCaptureColor, kCaptureLevel);
    } else if (scene == sequencer_.pendingScene()) {
      // Waiting for the top of the next bar: it blinks until it lands.
      color = blinkOn(state.nowMs) ? kSelectedColor : kFilledColor;
    } else if (scene == sequencer_.currentScene()) {
      color = kSelectedColor;
    } else {
      color = used ? kFilledColor : kEmptyColor;
    }
    frame.pads[scene] = color;
  }

  for (uint8_t column = 0; column < kGridCols; ++column) {
    const uint16_t track = state.trackPage * kTracksPerPage + column;
    if (track >= kNumTracks) continue;
    const uint8_t t = static_cast<uint8_t>(track);
    const Rgb color = trackColor(t);
    frame.pads[kFirstMutePad + column] =
        sequencer_.trackMuted(t) ? dim(color, kMutedLevel) : color;
  }
}

const char* SceneMode::padLabel(const UiState&, uint8_t pad) const {
  if (pad < kNumScenes) return kSceneLabels[pad];
  if (pad >= kFirstMutePad) return kMuteLabels[pad - kFirstMutePad];
  return nullptr;
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kLegendSceneColor = {30, 156, 255};
const Rgb kLegendTrackColor = {255, 130, 0};
const Rgb kLegendCaptureColor = {255, 43, 43};

const ModeLegend kSceneLegend = {
    "SHIFT + R2",
    "Scenes",
    {
        {1, 4, kLegendSceneColor, "rows 1-4", "32 scenes - blue held, green playing"},
        {8, 8, kLegendTrackColor, "row 8", "the 8 tracks of this page - tap to mute"},
        {0, 0, kLegendCaptureColor, "hold R7 pad", "captures what you are hearing"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"tap a scene launches it at the top of the next bar", "R2 back to pattern",
     "R5 + pad erase, R6 + pad + pad copy", NULL},
    3,
    {kLegendSceneColor, kLegendSceneColor, kLegendSceneColor, kLegendSceneColor,
     {}, {}, {}, kLegendTrackColor},
};

}  // namespace

const ModeLegend* SceneMode::legend(const UiState&) const { return &kSceneLegend; }

}  // namespace gx
