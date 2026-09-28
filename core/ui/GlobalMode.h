#pragma once

#include <stdint.h>

#include "engine/Sequencer.h"
#include "ui/DeviceStatus.h"
#include "ui/Mode.h"

namespace gx {

// Settings picked on the top row of global settings mode, one pad each.
enum GlobalSetting {
  kGlobalTempo = 0,    // pad 1
  kGlobalMidiChannel,  // pad 2: the selected track's MIDI channel
  kGlobalSwing,        // pad 3: how much every second step is held back
  kGlobalDevices,      // pad 4: what is plugged in, and a pad to go and look again
  kGlobalArp,          // pad 5: the selected track's arpeggiator
  kNumGlobalSettings,
};

// Shift + R1: settings. The top row picks one; nothing is picked until a pad is tapped, and
// the pick then stays for later visits.
//  - pad 1, tempo: shown in the 3x5 font on the rows below; fader 1 sets it, and pads 5 and 6
//    of the bottom row step it up and down by 1
//  - the last pad of the bottom row sends every fader and knob's CC again
//  - pad 3, swing: the same display and controls as the tempo, 50 (straight) to 75
//  - pad 2, where the selected track plays: rows 3 and 4 are MIDI channels 1-8 and 9-16,
//    row 5 the MIDI ports P1..P8 and rows 6 and 7 the internal instruments I1..I16. A track
//    uses a port or an instrument; the one in use lights in the track colour
//  - pad 4, devices: row 3 is the control surfaces and row 5 the MIDI ports P1..P8, each lit
//    where something answered, row 7 the clock source, and the first pad of the bottom row
//    looks again
//  - pad 5, the selected track's arpeggiator: row 3 is the mode, row 5 the rate and row 7 how
//    many octaves it climbs, each in the track's colour
class GlobalMode : public Mode {
 public:
  explicit GlobalMode(Sequencer& sequencer);

  // What the platform knows about the gear around it, for the devices page. Without one the
  // page shows everything as absent and its refresh pad does nothing.
  void setDeviceStatus(DeviceStatus* devices) { devices_ = devices; }

  void handlePad(UiState& state, uint8_t pad, bool pressed) override;
  bool handleFader(UiState& state, uint8_t fader, uint16_t value) override;
  void renderPads(const UiState& state, LedFrame& frame) const override;
  // The settings on the top row, then the pads of the picked setting.
  const char* padLabel(const UiState& state, uint8_t pad) const override;
  void reset() override;

  uint8_t selectedSetting() const { return selected_; }  // GlobalSetting, or kNoPad

 private:
  // Tempo and swing are both plain numbers on a fader, so they share the display, the fader
  // and the up and down pads; only the range and where the value lives differ.
  bool isNumberSetting() const;
  // The tempo page while someone else's clock is driving us: it still has a number to show,
  // but the number isn't ours to change.
  bool tempoIsExternal() const;
  bool isEditable() const { return isNumberSetting() && !tempoIsExternal(); }
  uint16_t value() const;
  void setValue(uint16_t value);
  uint16_t minValue() const;
  uint16_t maxValue() const;

  Sequencer& sequencer_;
  DeviceStatus* devices_;  // NULL on a build with nothing to report
  uint8_t selected_;
  uint8_t held_;  // bit 0: the up pad is held, bit 1: the down pad
};

}  // namespace gx
