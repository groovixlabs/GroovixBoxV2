#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Mode.h"

namespace gx {

// What a lane of 16 pads below the steps edits.
enum StepLane {
  kLaneVelocity = 0,
  kLaneGate,
  kLaneProbability,
  kLaneNudge,    // micro-timing: how far off the grid the step plays
  kLaneRatchet,  // how many times the step fires across its own length
  kNumLaneKinds,
};

static const uint8_t kNoLane = 0xFF;
static const uint8_t kMaxStepLanes = 2;

// Per-step parameters: Shift + R4 edits velocity and gate, Shift + R5 probability. The top four
// rows are the current page of steps: tap one to select it, hold several to edit them together.
// Below them each parameter has two rows, 16 pads read left to right and then along the second
// row:
//  - velocity: 8, 16 ... 127
//  - gate: one ramp of lengths, a pad each, shortest to longest: a sixth of a step up to five
//    sixths, then 1..6 steps, then 8, 10, 12, 14, 16. A tap is that length
//  - probability: sixteenths, 6% ... 100%
//  - micro-timing: a signed ramp, straight on the 9th pad - the first of the lane's second
//    row. Single ticks either side (a tick is a 24th of a step), then 4, 6, 8, 10 and 12 early
//    and 4, 6, 8 and 12 late; the pads light from the straight one out to the value
//  - ratchet: pad n fires the step n + 1 times across its own length, so two pads in is
//    32nd notes and eight is a roll. Only the first 8 pads of the lane are used.
// A mode can give its second lane a Shift face: while Shift is held those rows edit that
// parameter instead, which is how the ratchets share the row with the gate.
// Clear + step resets the mode's parameters and Duplicate + step + step copies them. Active steps
// glow brighter the higher the first parameter.
class StepParamsMode : public Mode {
 public:
  // The first lane goes on rows 5-6 and the second on rows 7-8; kNoLane leaves rows dark.
  // shiftLane replaces the second while Shift is held.
  StepParamsMode(Sequencer& sequencer, uint8_t firstLane, uint8_t secondLane,
                 uint8_t shiftLane = kNoLane);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  const ModeLegend* legend(const UiState& /*state*/) const override { return &legend_; }
  uint8_t displayValues(const UiState& state, DisplayValue* values) const override;
  void reset() override;

 private:
  void handleStep(UiState& state, uint8_t pad, bool pressed);
  // A lane pad (0..15) tapped with steps selected.
  void setLane(const UiState& state, uint8_t lane, uint8_t lanePad);
  uint8_t value(uint8_t lane, uint8_t track, uint16_t step) const;
  void setValue(uint8_t lane, uint8_t track, uint16_t step, uint8_t v);
  uint8_t stepBrightness(uint8_t track, uint16_t step) const;
  // The lowest selected step, whose values the lanes show, or kNoSlot.
  uint16_t shownStep(const UiState& state) const;

  Sequencer& sequencer_;
  // The lane a row shows right now: the second one has a Shift face, if it was given one.
  uint8_t laneAt(const UiState& state, uint8_t index) const;

  // Built once from the lanes this instance was given: the same class is the velocity/gate
  // mode and the probability/micro-timing one, so what its rows do differs per instance.
  ModeLegend legend_;

  uint8_t lanes_[kMaxStepLanes];  // StepLane
  uint8_t shiftLane_;             // what Shift puts on the second lane, or kNoLane
  uint8_t laneCount_;
  uint32_t selected_;   // bit n = step pad n selected
  uint32_t heldSteps_;  // bit n = step pad n held
};

}  // namespace gx
