#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Mode.h"

namespace gx {

// Shift + R7 opens the song: the scenes in the order they play.
//
//  - the top four rows are the 32 scenes, the palette to choose from
//  - the bottom four rows are the 32 song steps, each showing the colour of the scene it
//    plays; every step lasts kBarsPerSongStep bars, so a longer section is the same scene in
//    several steps
//  - hold a song step to see which scene it holds, and tap a scene while holding it to put
//    that scene there
//  - tap a song step on its own to go there and hear it
//  - Clear + a step empties it, Duplicate + step + step repeats a section
//
// Nothing plays the song yet: it is written here, and the transport will follow it once the
// sequencer counts bars.
class ArrangementMode : public Mode {
 public:
  explicit ArrangementMode(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  const ModeLegend* legend(const UiState& state) const override;
  const char* padLabel(const UiState& state, uint8_t pad) const override;
  void reset() override;

 private:
  Sequencer& sequencer_;
  uint8_t heldStep_;  // song step being held, or kNoPad
  bool assigned_;     // a scene was tapped while holding it, so releasing shouldn't jump
};

}  // namespace gx
