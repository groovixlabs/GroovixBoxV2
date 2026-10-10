#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Mode.h"

namespace gx {

// Shift + R4 opens scene mode; R4 returns to pattern mode.
//
// A scene is a song section — intro, verse, drop — remembered as which tracks are silent.
//  - the top four rows are 32 scene pads: tap one to launch it
//  - hold R7 (Record) and tap a pad to capture what you are hearing into it
//  - Clear + pad erases a scene, Duplicate + pad + pad copies one
//  - the bottom row mutes the eight tracks of the current track page, so a scene can be
//    shaped right here before it is captured
// Scenes are saved with the project; the mutes themselves belong to the session.
class SceneMode : public Mode {
 public:
  explicit SceneMode(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  const ModeLegend* legend(const UiState& state) const override;
  const char* padLabel(const UiState& state, uint8_t pad) const override;

 private:
  Sequencer& sequencer_;
};

}  // namespace gx
