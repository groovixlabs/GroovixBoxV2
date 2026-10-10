#include "ui/StepParamsMode.h"

#include "ui/Controls.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kStepRows = kStepsPerPage / kGridCols;  // the lanes start below the steps
const uint8_t kRowsPerLane = 2;
const uint8_t kLanePads = kRowsPerLane * kGridCols;  // 16 levels per parameter
const uint8_t kEmptyStepLevel = 14;
const Rgb kLastStepColor = {255, 0, 0};

const Rgb kLaneColors[kNumLaneKinds] = {
    {255, 120, 0},   // velocity
    {0, 200, 255},   // gate
    {170, 80, 255},  // probability
    {255, 60, 140},  // micro-timing
    {255, 230, 0},   // ratchet: yellow, so a roll never reads as a long gate
};

// Micro-timing is a ramp like the gate's, but signed and centred: single ticks either side of
// the grid, where push-and-drag feel lives, then wider rungs out to half a step. The outer
// rungs are what put triplets in reach - every 8th- and 16th-triplet position in a beat sits 8
// ticks from a step - and the far ends displace a hit by a 32nd, for flams and in-between hits.
const int8_t kNudgeRamp[kLanePads] = {
    -12, -10, -8, -6, -4, -3, -2, -1,  // early
    0,                                 // on the grid: the first pad of the lane's second row
    1,   2,   3,  4,  6,  8,  12,      // late
};
const uint8_t kStraightPad = 8;  // kNudgeRamp[kStraightPad] is 0
const uint8_t kStraightLevel = 45;  // the pad that means "on the grid", when it isn't the value

const uint8_t kBarLevel = 90;        // lane pads below the value
const uint8_t kLaneTrackLevel = 12;  // lane pads above it, so the lane's extent shows

uint32_t bit(uint8_t n) { return static_cast<uint32_t>(1) << n; }

// Velocity in sixteenths: 8, 16 ... 120, and 127 on the last pad.
uint8_t velocityForLevel(uint8_t level) {
  const unsigned velocity = (level + 1u) * 8u;
  return static_cast<uint8_t>(velocity > kMaxMidiValue ? kMaxMidiValue : velocity);
}

uint8_t levelForVelocity(uint8_t velocity) {
  if (velocity == 0) return 0;
  const unsigned level = (velocity - 1u) / 8u;
  return static_cast<uint8_t>(level < kLanePads ? level : kLanePads - 1);
}

// Probability in sixteenths: 6, 13, 19 ... 94 and 100 percent.
uint8_t probabilityForLevel(uint8_t level) {
  return static_cast<uint8_t>(((level + 1u) * kMaxProbability + kLanePads / 2) / kLanePads);
}

uint8_t levelForProbability(uint8_t probability) {
  const unsigned rounded = (probability * kLanePads + kMaxProbability / 2u) / kMaxProbability;
  if (rounded == 0) return 0;
  return static_cast<uint8_t>(rounded - 1 < kLanePads ? rounded - 1 : kLanePads - 1);
}

// The gate lane is one ramp of lengths, shortest to longest, a pad each. Every other lane shows
// its whole range at once and a tap picks a value; the gate used to hide the part-step lengths
// behind a second tap on the lit pad, which read as nothing at all until you found it.
// The resolution is where the ear is: sixths inside the step, where staccato lives, then every
// step, then every other step - there is little to tell apart between 13 steps and 14.
const uint8_t kStepUnits = kGateUnitsPerStep;
const uint8_t kGateRamp[kLanePads] = {
    1, 2, 3, 4, 5,                                                    // a sixth .. five sixths
    1 * kStepUnits,  2 * kStepUnits,  3 * kStepUnits,                 // 1, 2, 3 steps
    4 * kStepUnits,  5 * kStepUnits,  6 * kStepUnits,                 // 4, 5, 6
    8 * kStepUnits,  10 * kStepUnits, 12 * kStepUnits,                // 8, 10, 12
    14 * kStepUnits, 16 * kStepUnits,                                 // 14, 16
};

// The pad a gate sits on: the longest one that is no longer than the gate. A gate off the ramp -
// an older project's, or one Duplicate carried over - lands on the pad below it rather than
// nowhere.
uint8_t gatePadFor(uint8_t ticks) {
  uint8_t pad = 0;
  for (uint8_t i = 0; i < kLanePads; ++i) {
    if (kGateRamp[i] <= ticks) pad = i;
  }
  return pad;
}

// The pad a nudge sits on: the nearest rung, and of two equally near the one closer to the
// grid, so a value off the ramp never reads as more shifted than it is.
uint8_t nudgePadFor(int8_t ticks) {
  uint8_t best = 0;
  for (uint8_t i = 1; i < kLanePads; ++i) {
    const int16_t err = static_cast<int16_t>(ticks - kNudgeRamp[i]);
    const int16_t bestErr = static_cast<int16_t>(ticks - kNudgeRamp[best]);
    const int16_t absErr = err < 0 ? static_cast<int16_t>(-err) : err;
    const int16_t absBest = bestErr < 0 ? static_cast<int16_t>(-bestErr) : bestErr;
    const int16_t mag = kNudgeRamp[i] < 0 ? -kNudgeRamp[i] : kNudgeRamp[i];
    const int16_t bestMag = kNudgeRamp[best] < 0 ? -kNudgeRamp[best] : kNudgeRamp[best];
    if (absErr < absBest || (absErr == absBest && mag < bestMag)) best = i;
  }
  return best;
}

uint8_t defaultValue(uint8_t lane) {
  switch (lane) {
    case kLaneVelocity:
      return kDefaultVelocity;
    case kLaneGate:
      return kDefaultGateUnits;
    case kLaneNudge:
      return kStraightPad;  // lanes carry the nudge as its pad, 0..15
    case kLaneRatchet:
      return kDefaultRatchet - 1;  // the first pad: it fires once
    default:
      return kMaxProbability;
  }
}

// A lane as a bar up to its value.
Rgb barPad(uint8_t lane, uint8_t pad, uint8_t valuePad) {
  const Rgb color = kLaneColors[lane];
  if (pad == valuePad) return color;
  return dim(color, pad < valuePad ? kBarLevel : kLaneTrackLevel);
}

// The micro-timing lane: lit from the straight pad out to the value, so early reads to the
// left of centre and late to the right.
Rgb nudgePad(uint8_t pad, uint8_t valuePad) {
  const Rgb color = kLaneColors[kLaneNudge];
  if (pad == valuePad) return color;
  const bool between = valuePad < kStraightPad ? (pad > valuePad && pad <= kStraightPad)
                                               : (pad >= kStraightPad && pad < valuePad);
  if (between) return dim(color, kBarLevel);
  if (pad == kStraightPad) return dim(color, kStraightLevel);
  return dim(color, kLaneTrackLevel);
}

}  // namespace

static_assert(kStepRows + kMaxStepLanes * kRowsPerLane == kGridRows,
              "two lanes of two rows fill the grid below the steps");
static_assert(kGateUnitsPerStep == 6, "the ramp's first five pads are sixths of a step");
static_assert(16 * kGateUnitsPerStep == kMaxGateUnits, "the ramp's last pad is the longest gate");

// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kStepsColor = {54, 214, 95};    // the steps, the same green they light
const Rgb kVelocityColor = {255, 225, 0};
const Rgb kGateColor = {0, 215, 200};     // cyan, so it isn't mistaken for the steps' green
const Rgb kProbColor = {255, 60, 170};
const Rgb kNudgeColor = {255, 189, 108};
const Rgb kShiftColor = {160, 60, 255};

struct LaneText {
  const char* label;
  const char* text;
  Rgb color;
  const char* valueKey;
};

// One entry per StepLane, in the order the enum declares them.
const LaneText kLaneText[kNumLaneKinds] = {
    {"velocity", "velocity, 16 levels 8 -> 127", kVelocityColor, "VELOCITY"},
    {"gate", "gate, 1/6 step to 16 steps", kGateColor, "GATE"},
    {"chance", "how often it plays, 6% -> 100%", kProbColor, "CHANCE"},
    {"micro-timing", "how far off the grid it plays, in ticks", kNudgeColor, "NUDGE"},
    {"ratchet", "pad n fires the step n+1 times", kShiftColor, "RATCHET"},
};

// The gate lane as words: one per rung of kGateRamp, so the screen says what the pads mean.
const char* const kGateNames[kLanePads] = {
    "1/6 step", "2/6 step", "1/2 step", "4/6 step", "5/6 step",
    "1 step",   "2 steps",  "3 steps",  "4 steps",  "5 steps",
    "6 steps",  "8 steps",  "10 steps", "12 steps", "14 steps", "16 steps",
};

// The micro-timing lane as words, one per rung of kNudgeRamp.
const char* const kNudgeNames[kLanePads] = {
    "-12", "-10", "-8", "-6", "-4", "-3", "-2", "-1",
    "straight", "+1", "+2", "+3", "+4", "+6", "+8", "+12",
};

}  // namespace

StepParamsMode::StepParamsMode(Sequencer& sequencer, uint8_t firstLane, uint8_t secondLane,
                               uint8_t shiftLane)
    : sequencer_(sequencer), shiftLane_(shiftLane), laneCount_(0), selected_(0), heldSteps_(0) {
  lanes_[0] = kNoLane;
  lanes_[1] = kNoLane;
  if (firstLane < kNumLaneKinds) lanes_[laneCount_++] = firstLane;
  if (secondLane < kNumLaneKinds) lanes_[laneCount_++] = secondLane;

  // The legend follows the lanes this instance was given, so the velocity/gate mode and the
  // probability/micro-timing one each describe themselves without a second class.
  legend_.key = firstLane == kLaneVelocity ? "R2" : "R3";
  legend_.name = firstLane == kLaneVelocity ? "Step parameters" : "Probability";
  legend_.numBands = 0;
  legend_.bands[legend_.numBands].firstRow = 1;
  legend_.bands[legend_.numBands].lastRow = 4;
  legend_.bands[legend_.numBands].color = kStepsColor;
  legend_.bands[legend_.numBands].label = "rows 1-4";
  legend_.bands[legend_.numBands].text = "the 32 steps - brighter is more";
  ++legend_.numBands;
  for (uint8_t i = 0; i < laneCount_; ++i) {
    const LaneText& lane = kLaneText[lanes_[i]];
    DisplayBand& band = legend_.bands[legend_.numBands++];
    band.firstRow = static_cast<uint8_t>(5 + i * 2);
    band.lastRow = static_cast<uint8_t>(6 + i * 2);
    band.color = lane.color;
    band.label = i == 0 ? "rows 5-6" : "rows 7-8";
    band.text = lane.text;
  }
  if (shiftLane_ < kNumLaneKinds) {
    DisplayBand& band = legend_.bands[legend_.numBands++];
    band.firstRow = 0;  // a modifier, not rows of its own
    band.lastRow = 0;
    band.color = kShiftColor;
    band.label = "SHIFT 7-8";
    band.text = kLaneText[shiftLane_].text;
  }
  legend_.numMods = 0;
  legend_.mods[legend_.numMods++] = "SHIFT + top row: step page";
  legend_.mods[legend_.numMods++] = "R5 + step reset";
  legend_.mods[legend_.numMods++] = "R1 back to note";
  for (uint8_t row = 0; row < kGridRows; ++row) {
    legend_.rowColor[row] = row < 4 ? kStepsColor
                            : row < 6 ? kLaneText[lanes_[0]].color
                            : laneCount_ > 1 ? kLaneText[lanes_[1]].color
                                             : Rgb();
  }
}

uint8_t StepParamsMode::displayValues(const UiState& state, DisplayValue* values) const {
  const uint16_t step = shownStep(state);
  if (step == kNoSlot) return 0;
  uint8_t count = 0;
  values[count].key = "STEP";
  values[count].text = NULL;
  values[count].number = static_cast<uint16_t>(step + 1);
  values[count].suffix = NULL;
  ++count;
  for (uint8_t i = 0; i < laneCount_ && count < kMaxDisplayValues; ++i) {
    const uint8_t lane = lanes_[i];
    DisplayValue& v = values[count++];
    v.key = kLaneText[lane].valueKey;
    v.text = NULL;
    v.suffix = NULL;
    v.number = value(lane, state.track, step);
    // Two lanes are rungs rather than counts, so their pad reads better as a word.
    if (lane == kLaneGate) v.text = kGateNames[gatePadFor(v.number)];
    if (lane == kLaneNudge) v.text = kNudgeNames[nudgePadFor(static_cast<int8_t>(v.number))];
    if (lane == kLaneProbability) v.suffix = "%";
  }
  if (shiftLane_ == kLaneRatchet && count < kMaxDisplayValues) {
    const uint8_t hits = sequencer_.stepRatchet(state.track, step);
    DisplayValue& v = values[count++];
    v.key = "RATCHET";
    v.text = hits > 1 ? NULL : "off";
    v.number = hits;
    v.suffix = hits > 1 ? " hits" : NULL;
  }
  return count;
}

// Shift turns the second lane into its other face - the ratchets, over the gate - so a step's
// timing is all in one place without a mode of its own.
uint8_t StepParamsMode::laneAt(const UiState& state, uint8_t index) const {
  if (index >= laneCount_) return kNoLane;
  if (index == 1 && state.shiftHeld && shiftLane_ < kNumLaneKinds) return shiftLane_;
  return lanes_[index];
}

void StepParamsMode::reset() {
  selected_ = 0;
  heldSteps_ = 0;
}

void StepParamsMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (pad < kStepsPerPage) {
    handleStep(state, pad, pressed);
    return;
  }
  if (!pressed || pad >= kNumPads) return;
  const uint8_t row = static_cast<uint8_t>(pad / kGridCols - kStepRows);
  const uint8_t lane = laneAt(state, static_cast<uint8_t>(row / kRowsPerLane));
  if (lane == kNoLane) return;
  setLane(state, lane, static_cast<uint8_t>((row % kRowsPerLane) * kGridCols + pad % kGridCols));
}

void StepParamsMode::handleStep(UiState& state, uint8_t pad, bool pressed) {
  const uint32_t mask = bit(pad);
  if (!pressed) {
    heldSteps_ &= ~mask;
    return;
  }

  const uint8_t track = state.track;
  const uint16_t step = static_cast<uint16_t>(state.stepPage * kStepsPerPage + pad);
  uint16_t from = kNoSlot;
  // Clear and Duplicate act on everything the mode owns, the Shift face included: Shift + R5
  // opens another mode, so a ratchet could not be reached by holding Shift as well.
  if (state.clearHeld) {
    for (uint8_t i = 0; i < laneCount_; ++i) setValue(lanes_[i], track, step, defaultValue(lanes_[i]));
    if (shiftLane_ < kNumLaneKinds) {
      setValue(shiftLane_, track, step, defaultValue(shiftLane_));
    }
  } else if (state.duplicateHeld) {
    if (takeDuplicateTarget(state, step, from)) {
      for (uint8_t i = 0; i < laneCount_; ++i) {
        setValue(lanes_[i], track, step, value(lanes_[i], track, from));
      }
      if (shiftLane_ < kNumLaneKinds) {
        setValue(shiftLane_, track, step, value(shiftLane_, track, from));
      }
    }
  } else {
    // With other steps held this one joins the selection; on its own it replaces it.
    selected_ = heldSteps_ ? (selected_ | mask) : mask;
    heldSteps_ |= mask;
  }
}

void StepParamsMode::setLane(const UiState& state, uint8_t lane, uint8_t lanePad) {
  if (selected_ == 0) return;
  const uint8_t track = state.track;
  for (uint8_t pad = 0; pad < kStepsPerPage; ++pad) {
    if (!(selected_ & bit(pad))) continue;
    const uint16_t step = static_cast<uint16_t>(state.stepPage * kStepsPerPage + pad);
    switch (lane) {
      case kLaneVelocity:
        sequencer_.setStepVelocity(track, step, velocityForLevel(lanePad));
        break;
      case kLaneGate:
        sequencer_.setStepGate(track, step, kGateRamp[lanePad]);
        break;
      case kLaneProbability:
        sequencer_.setStepProbability(track, step, probabilityForLevel(lanePad));
        break;
      case kLaneNudge:
        sequencer_.setStepNudge(track, step, kNudgeRamp[lanePad]);
        break;
      case kLaneRatchet:
        sequencer_.setStepRatchet(track, step, static_cast<uint8_t>(lanePad + 1));
        break;
    }
  }
}

uint8_t StepParamsMode::value(uint8_t lane, uint8_t track, uint16_t step) const {
  switch (lane) {
    case kLaneVelocity:
      return sequencer_.stepVelocity(track, step);
    case kLaneGate:
      return sequencer_.stepGate(track, step);
    case kLaneNudge:  // as its pad, so the lane's plumbing stays unsigned
      return nudgePadFor(sequencer_.stepNudge(track, step));
    case kLaneRatchet:
      return static_cast<uint8_t>(sequencer_.stepRatchet(track, step) - 1);
    default:
      return sequencer_.stepProbability(track, step);
  }
}

void StepParamsMode::setValue(uint8_t lane, uint8_t track, uint16_t step, uint8_t v) {
  switch (lane) {
    case kLaneVelocity:
      sequencer_.setStepVelocity(track, step, v);
      break;
    case kLaneGate:
      sequencer_.setStepGate(track, step, v);
      break;
    case kLaneProbability:
      sequencer_.setStepProbability(track, step, v);
      break;
    case kLaneNudge:
      sequencer_.setStepNudge(track, step, kNudgeRamp[v]);
      break;
    case kLaneRatchet:
      sequencer_.setStepRatchet(track, step, static_cast<uint8_t>(v + 1));
      break;
  }
}

// An active step glows brighter the higher its first parameter: a hard or likely step shows at
// a glance.
uint8_t StepParamsMode::stepBrightness(uint8_t track, uint16_t step) const {
  if (laneCount_ == 0) return 255;
  const uint8_t lane = lanes_[0];
  const unsigned maximum = lane == kLaneVelocity ? kMaxMidiValue
                           : lane == kLaneGate   ? kMaxGateUnits
                                                 : kMaxProbability;
  return static_cast<uint8_t>(40u + value(lane, track, step) * 215u / maximum);
}

uint16_t StepParamsMode::shownStep(const UiState& state) const {
  for (uint8_t pad = 0; pad < kStepsPerPage; ++pad) {
    if (selected_ & bit(pad)) return static_cast<uint16_t>(state.stepPage * kStepsPerPage + pad);
  }
  return kNoSlot;
}

void StepParamsMode::renderPads(const UiState& state, LedFrame& frame) const {
  const uint8_t track = state.track;
  const Rgb color = trackColor(track);
  const uint16_t length = sequencer_.trackLength(track);
  const bool playing = sequencer_.playing();
  const uint16_t playhead = sequencer_.playhead(track);
  const uint16_t firstStep = static_cast<uint16_t>(state.stepPage * kStepsPerPage);

  for (uint8_t pad = 0; pad < kStepsPerPage; ++pad) {
    const uint16_t step = static_cast<uint16_t>(firstStep + pad);
    const bool active = sequencer_.stepActive(track, step);
    if (step >= length) {
      frame.pads[pad] = kBlack;
    } else if (selected_ & bit(pad)) {
      frame.pads[pad] = kSelectedColor;
    } else if (state.duplicateHeld && step == state.duplicateSource) {
      frame.pads[pad] = kCopySourceColor;
    } else if (playing && step == playhead) {
      frame.pads[pad] = playheadColor(sequencer_.recording(), active);
    } else if (step + 1 == length) {
      frame.pads[pad] = kLastStepColor;
    } else if (active) {
      frame.pads[pad] = dim(color, stepBrightness(track, step));
    } else {
      frame.pads[pad] = dim(color, kEmptyStepLevel);
    }
  }

  // Lanes: the selected step's values; with nothing selected only the lanes' extent shows.
  const uint16_t shown = shownStep(state);
  for (uint8_t lane = 0; lane < kMaxStepLanes; ++lane) {
    for (uint8_t lanePad = 0; lanePad < kLanePads; ++lanePad) {
      const uint8_t row = static_cast<uint8_t>(kStepRows + lane * kRowsPerLane + lanePad / kGridCols);
      Rgb c = kBlack;
      const uint8_t kind = laneAt(state, lane);
      if (kind != kNoLane) {
        if (kind == kLaneRatchet && lanePad >= kMaxRatchet) {
          c = kBlack;  // a step fires at most kMaxRatchet times, so the rest is dark
        } else if (shown == kNoSlot) {
          c = dim(kLaneColors[kind], kLaneTrackLevel);
        } else if (kind == kLaneGate) {
          c = barPad(kind, lanePad, gatePadFor(sequencer_.stepGate(track, shown)));
        } else if (kind == kLaneNudge) {
          c = nudgePad(lanePad, value(kind, track, shown));
        } else if (kind == kLaneRatchet) {
          c = barPad(kind, lanePad, value(kind, track, shown));
        } else {
          const uint8_t v = value(kind, track, shown);
          c = barPad(kind, lanePad, kind == kLaneVelocity ? levelForVelocity(v) : levelForProbability(v));
        }
      }
      frame.pads[padIndex(row, lanePad % kGridCols)] = c;
    }
  }
}

}  // namespace gx
