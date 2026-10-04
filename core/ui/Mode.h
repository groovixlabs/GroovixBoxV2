#pragma once

#include <stdint.h>

#include "ui/DisplayFrame.h"
#include "ui/LedFrame.h"
#include "ui/UiState.h"

namespace gx {

// Pad labels are at most kPadLabelLines lines, split by '\n', of kPadLabelLineChars each.
static const uint8_t kPadLabelLines = 3;
static const uint8_t kPadLabelLineChars = 7;

// What the pad grid does in one mode (project, pattern, note, preset).
class Mode {
 public:
  virtual void handlePad(UiState& state, uint8_t pad, bool pressed) = 0;
  // Fader fader moved to value, 0..kFaderMax. Returns true if the mode used it, which keeps
  // it from also sending its MIDI CC. Most modes don't use the faders.
  virtual bool handleFader(UiState& /*state*/, uint8_t /*fader*/, uint16_t /*value*/) {
    return false;
  }
  virtual void renderPads(const UiState& state, LedFrame& frame) const = 0;
  // What a pad does, for surfaces that can print on their pads (the simulator), to help
  // learn a mode. nullptr for no label; most modes have none.
  virtual const char* padLabel(const UiState& /*state*/, uint8_t /*pad*/) const { return nullptr; }
  // What this mode's rows are for, so a screen beside the instrument can say it while the
  // sequencer is stopped and you are editing. NULL for a mode with nothing to explain; the
  // one returned is a static constant, never built per call.
  virtual const ModeLegend* legend(const UiState& /*state*/) const { return nullptr; }
  // Values worth spelling out below the legend - the selected step's velocity, the track's
  // key. Fills at most kMaxDisplayValues and returns how many it wrote.
  virtual uint8_t displayValues(const UiState& /*state*/, DisplayValue* /*values*/) const {
    return 0;
  }
  // Drops held-pad state. Called when leaving the mode or changing track.
  virtual void reset() {}

 protected:
  ~Mode() {}  // see EventSink
};

}  // namespace gx
