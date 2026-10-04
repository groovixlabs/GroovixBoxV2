#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/Mode.h"

namespace gx {

// Shift + R3 opens scale mode and R3 returns to note mode. The top two rows are a piano
// for the root note (black keys above white keys, C to C) and the bottom four rows hold
// the scales. The selected root and scale light green; note mode's keyboard then offers
// only notes of that scale. The last two pads of the bottom row set the selected track's
// keyboard to drum pads (amber when on) or its own scale (the track colour when on); tapping
// the pad in use goes back to the song's scale. While a track has its own scale, the piano
// and scale list edit only that track and show its picks in the track colour, with the
// song's key as a faint green hint. Pad 6 of the bottom row turns the selected track's piano
// The last four pads of the bottom row say what the keys play: TRIAD and 7TH, then Drums and
// Own scale. With TRIAD or 7TH lit, a single key of the note keyboard plays that many notes of
// the scale, stacked in thirds; tapping the lit one goes back to one key, one note. The first
// pad of the row turns the piano roll on or off (white when on): it changes the grid rather
// than the keys, so it sits on its own at the other end.
class ScaleMode : public Mode {
 public:
  explicit ScaleMode(Sequencer& sequencer);

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  const ModeLegend* legend(const UiState& state) const override;
  // Note names on the piano, scale names, and the track's keyboard pads.
  const char* padLabel(const UiState& state, uint8_t pad) const override;

 private:
  Sequencer& sequencer_;
};

}  // namespace gx
