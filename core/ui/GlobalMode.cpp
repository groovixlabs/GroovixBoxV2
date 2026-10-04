#include "ui/GlobalMode.h"

#include "ui/Controls.h"
#include "ui/Font3x5.h"
#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kValueTopRow = 2;   // the value's five rows, below a blank row
const uint8_t kIdleSettingLevel = 60;
// MIDI channel setting: rows 3 and 4 are channels 1-8 and 9-16, row 5 the MIDI ports P1..P8
// and rows 6 and 7 the internal instruments I1..I16. A track uses a port or an instrument, so
// whichever it is on lights in the track colour and the other row dims further.
const uint8_t kFirstChannelPad = 2 * kGridCols;
const uint8_t kIdleChannelLevel = 35;
const uint8_t kFirstPortPad = 4 * kGridCols;
const uint8_t kFirstInstrumentPad = 5 * kGridCols;
const uint8_t kUnusedRouteLevel = 12;  // the routing the track isn't using
// The channel, port and instrument in use are lifted towards white rather than left at the
// plain track colour. A track colour's brightness follows its hue - yellow reads about twice
// as bright as blue - so the raw colour made the setting in use look dim on half the tracks,
// dimmer than the green this surface uses everywhere else to mean "this one". Lifting evens
// them out and keeps the hue, so the pad still says which track is being set.
const uint8_t kRouteInUseLift = 120;
// Bottom row: coarse in columns 2 and 3, fine in columns 5 and 6. Two pairs rather than a
// fader, because a fader covers 20..300 BPM in its travel - about three BPM a pixel - which
// can reach a tempo but never quite the one you wanted. Coarse gets you near, fine lands.
const uint8_t kStepRow = kGridRows - 1;
const uint8_t kCoarseUpPad = kStepRow * kGridCols + 1;
const uint8_t kCoarseDownPad = kStepRow * kGridCols + 2;
const uint8_t kUpPad = kStepRow * kGridCols + 4;
const uint8_t kDownPad = kStepRow * kGridCols + 5;
const int16_t kCoarseStep = 10;
// Sends every fader and knob's CC again, so the gear matches the panel after a change of
// project. An action rather than a setting, so it sits on the bottom row with the steppers.
const uint8_t kSendCcPad = kStepRow * kGridCols + 7;
const uint8_t kSendCcLevel = 70;
const uint8_t kHeldUp = 1;
const uint8_t kHeldDown = 2;
const uint8_t kHeldCoarseUp = 4;
const uint8_t kHeldCoarseDown = 8;

// The four stepper pads, as how far each moves the value and which bit marks it held.
struct Stepper {
  uint8_t pad;
  int16_t step;
  uint8_t heldBit;
};
const Stepper kSteppers[] = {
    {kCoarseUpPad, kCoarseStep, kHeldCoarseUp},
    {kCoarseDownPad, -kCoarseStep, kHeldCoarseDown},
    {kUpPad, 1, kHeldUp},
    {kDownPad, -1, kHeldDown},
};
const uint8_t kNumSteppers = sizeof(kSteppers) / sizeof(kSteppers[0]);

const Stepper* stepperFor(uint8_t pad) {
  for (uint8_t i = 0; i < kNumSteppers; ++i) {
    if (kSteppers[i].pad == pad) return &kSteppers[i];
  }
  return 0;
}
const uint8_t kStepPadLevel = 70;
const uint8_t kStepPadLimitLevel = 20;  // the value can't go further this way

const char* const kSettingLabels[kNumGlobalSettings] = {"TEMPO", "MIDI\nCH", "SWING",
                                                        "DEVI-\nCES", "ARP"};

// Arp page: the mode on row 3, the rate on row 5 and the octaves on row 7 - the same rows the
// routing page uses, so the eye lands in the same places.
const uint8_t kFirstArpModePad = 2 * kGridCols;
const uint8_t kFirstArpRatePad = 4 * kGridCols;
const uint8_t kFirstArpOctavePad = 6 * kGridCols;
const char* const kArpModeLabels[kNumArpModes] = {"OFF",  "UP",     "DOWN",  "UP\nDOWN",
                                                  "DOWN\nUP", "AS\nPLAYED", "RAN-\nDOM"};
// The note each rate plays, with a step being a sixteenth.
const char* const kArpRateLabels[kNumArpRates] = {"1/8",  "1/8T", "1/16", "1/16T",
                                                  "1/32", "1/32T", "1/64", "1/64T"};
const char* const kArpOctaveLabels[kMaxArpOctaves] = {"1 OCT", "2 OCT", "3 OCT", "4 OCT"};

// Devices page: row 3 is the control surfaces, row 5 the MIDI ports - the same row the
// routing page puts them on, so a port is in the same place whichever page you are on.
const uint8_t kFirstDevicePad = 2 * kGridCols;
const uint8_t kRefreshPad = kStepRow * kGridCols;  // bottom row, first pad
const uint8_t kAbsentLevel = 18;                   // a surface or port with nothing on it
const uint8_t kRefreshLevel = 70;
const uint8_t kRefreshWaitingLevel = 255;  // something has turned up that the ports could use
const char* const kDeviceLabels[DeviceStatus::kNumStatusDevices] = {"PADS", "MIXER", "KEYS"};
// The clock source, on row 7 of the devices page: it says how this box is wired to the rest
// of the rig, which is what that page is for, and unlike the tempo it is not part of the
// project. The lit pad is green while an outside clock is really driving us and white while
// our own is, so AUTO shows which of the two it has settled on.
const uint8_t kFirstClockPad = 6 * kGridCols;
const char* const kClockLabels[kNumClockSources] = {"INT", "AUTO", "EXT"};
const char* const kPortLabels[kNumMidiPorts] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
const char* const kInstrumentLabels[kNumInstruments] = {
    "I1", "I2",  "I3",  "I4",  "I5",  "I6",  "I7",  "I8",
    "I9", "I10", "I11", "I12", "I13", "I14", "I15", "I16"};
const char* const kChannelLabels[kNumMidiChannels] = {"1", "2",  "3",  "4",  "5",  "6",  "7",  "8",
                                                      "9", "10", "11", "12", "13", "14", "15", "16"};

// Three 3-wide digits need 9 columns and the grid has 8. The hundreds digit of a tempo is
// only ever 1 to 3, so it gets a 2-wide glyph; with no room for gaps, neighbouring digits
// alternate colour instead.
const uint8_t kNarrowWidth = 2;
const uint8_t kNarrowDigits[3][kGlyphHeight] = {
    // Rows from the top; bit 1 = left column, bit 0 = right column.
    {1, 3, 1, 1, 1},  // 1
    {3, 1, 3, 2, 3},  // 2
    {3, 1, 3, 1, 3},  // 3
};
const Rgb kDigitColors[2] = {{255, 255, 255}, {255, 140, 0}};

static_assert(kNarrowWidth + 2 * kGlyphWidth == kGridCols, "three digits fill the row");
static_assert(kValueTopRow + kGlyphHeight <= kStepRow,
              "the value fits between the settings and the step pads");
static_assert(kMaxBpm < 400, "a tempo's hundreds digit needs a narrow glyph");
static_assert(kFirstChannelPad + kNumMidiChannels <= kFirstPortPad,
              "the channel pads sit above the port pads");
static_assert(kFirstPortPad + kNumMidiPorts <= kFirstInstrumentPad,
              "the port pads sit above the instrument pads");
static_assert(kFirstInstrumentPad + kNumInstruments <= kStepRow * kGridCols,
              "the instrument pads sit above the bottom row");
static_assert(kNumMidiPorts <= kGridCols, "the ports fit on one row");
static_assert(kTracksPerMidiPort == kTracksPerPage,
              "a track page starts on its own port, so the pages and ports line up");

void drawDigit(LedFrame& frame, uint8_t digit, uint8_t firstCol, Rgb color) {
  const uint16_t glyph = kFont3x5['0' + digit - kFontFirstChar];
  for (uint8_t row = 0; row < kGlyphHeight; ++row) {
    for (uint8_t col = 0; col < kGlyphWidth; ++col) {
      const int shift = (kGlyphHeight - 1 - row) * kGlyphWidth + (kGlyphWidth - 1 - col);
      if ((glyph >> shift) & 1) {
        frame.pads[padIndex(static_cast<uint8_t>(kValueTopRow + row),
                            static_cast<uint8_t>(firstCol + col))] = color;
      }
    }
  }
}

Rgb stepPadColor(bool held, bool atLimit) {
  if (held) return kWhite;
  return dim(kWhite, atLimit ? kStepPadLimitLevel : kStepPadLevel);
}

// Draws 0..399 across the value rows: a narrow hundreds digit (blank below 100), then tens
// and units.
void drawNumber(LedFrame& frame, uint16_t value) {
  const uint16_t hundreds = value / 100;
  if (hundreds >= 1 && hundreds <= 3) {
    for (uint8_t row = 0; row < kGlyphHeight; ++row) {
      for (uint8_t col = 0; col < kNarrowWidth; ++col) {
        if ((kNarrowDigits[hundreds - 1][row] >> (kNarrowWidth - 1 - col)) & 1) {
          frame.pads[padIndex(static_cast<uint8_t>(kValueTopRow + row), col)] = kDigitColors[0];
        }
      }
    }
  }
  drawDigit(frame, static_cast<uint8_t>(value / 10 % 10), kNarrowWidth, kDigitColors[1]);
  drawDigit(frame, static_cast<uint8_t>(value % 10), kNarrowWidth + kGlyphWidth, kDigitColors[0]);
}

}  // namespace

GlobalMode::GlobalMode(Sequencer& sequencer)
    : sequencer_(sequencer), devices_(nullptr), selected_(kNoPad), held_(0) {}

void GlobalMode::reset() { held_ = 0; }

bool GlobalMode::isNumberSetting() const {
  return selected_ == kGlobalTempo || selected_ == kGlobalSwing;
}

bool GlobalMode::tempoIsExternal() const {
  return selected_ == kGlobalTempo && sequencer_.followingExternal();
}

uint16_t GlobalMode::value() const {
  if (selected_ == kGlobalSwing) return sequencer_.swing();
  // While following, show the tempo we are actually playing at, measured from the clock
  // coming in - the project's own bpm is not driving anything.
  if (tempoIsExternal() && sequencer_.externalBpm() != 0) return sequencer_.externalBpm();
  return sequencer_.bpm();
}

void GlobalMode::setValue(uint16_t value) {
  if (selected_ == kGlobalSwing) {
    sequencer_.setSwing(static_cast<uint8_t>(value));
  } else {
    sequencer_.setBpm(value);
  }
}

uint16_t GlobalMode::minValue() const { return selected_ == kGlobalSwing ? kMinSwing : kMinBpm; }

uint16_t GlobalMode::maxValue() const { return selected_ == kGlobalSwing ? kMaxSwing : kMaxBpm; }

void GlobalMode::handlePad(UiState& state, uint8_t pad, bool pressed) {
  if (const Stepper* stepper = stepperFor(pad)) {
    if (!pressed) {
      held_ = static_cast<uint8_t>(held_ & ~stepper->heldBit);
      return;
    }
    if (!isEditable()) return;
    held_ = static_cast<uint8_t>(held_ | stepper->heldBit);
    // Clamped, not refused: a coarse step near the end should still take you to the end
    // rather than do nothing because ten would overshoot.
    const int32_t wanted = static_cast<int32_t>(value()) + stepper->step;
    const int32_t low = minValue(), high = maxValue();
    const int32_t landed = wanted < low ? low : wanted > high ? high : wanted;
    if (landed != static_cast<int32_t>(value())) setValue(static_cast<uint16_t>(landed));
    return;
  }
  if (!pressed) return;
  if (pad == kSendCcPad) {
    state.sendAllRequested = true;  // the controller owns the control map, so it does it
    return;
  }
  if (selected_ == kGlobalDevices && pad >= kFirstClockPad &&
      pad - kFirstClockPad < kNumClockSources) {
    sequencer_.setClockSource(static_cast<uint8_t>(pad - kFirstClockPad));
    return;
  }
  if (selected_ == kGlobalDevices && pad == kRefreshPad) {
    if (devices_) devices_->refreshDevices();
    return;
  }
  if (selected_ == kGlobalArp && pad >= kFirstArpModePad) {
    if (pad < kFirstArpModePad + kNumArpModes) {
      sequencer_.setTrackArpMode(state.track, static_cast<uint8_t>(pad - kFirstArpModePad));
      return;
    }
    if (pad >= kFirstArpRatePad && pad < kFirstArpRatePad + kNumArpRates) {
      sequencer_.setTrackArpRate(state.track, static_cast<uint8_t>(pad - kFirstArpRatePad));
      return;
    }
    if (pad >= kFirstArpOctavePad && pad < kFirstArpOctavePad + kMaxArpOctaves) {
      sequencer_.setTrackArpOctaves(state.track,
                                    static_cast<uint8_t>(pad - kFirstArpOctavePad + 1));
      return;
    }
  }
  if (pad < kNumGlobalSettings) {
    selected_ = pad;
  } else if (selected_ == kGlobalMidiChannel && pad >= kFirstChannelPad &&
             pad < kFirstChannelPad + kNumMidiChannels) {
    sequencer_.setTrackMidiChannel(state.track, static_cast<uint8_t>(pad - kFirstChannelPad));
  } else if (selected_ == kGlobalMidiChannel && pad >= kFirstPortPad &&
             pad < kFirstPortPad + kNumMidiPorts) {
    sequencer_.setTrackMidiPort(state.track, static_cast<uint8_t>(pad - kFirstPortPad));
  } else if (selected_ == kGlobalMidiChannel && pad >= kFirstInstrumentPad &&
             pad < kFirstInstrumentPad + kNumInstruments) {
    // Tapping the instrument the track is already on sends it back to its MIDI port.
    const uint8_t instrument = static_cast<uint8_t>(pad - kFirstInstrumentPad);
    const bool inUse = sequencer_.trackInstrument(state.track) == instrument;
    sequencer_.setTrackInstrument(state.track, inUse ? kNoInstrument : instrument);
  }
}

const char* GlobalMode::padLabel(const UiState&, uint8_t pad) const {
  if (pad < kNumGlobalSettings) return kSettingLabels[pad];
  if (pad == kSendCcPad) return "SEND\nCC";
  if (selected_ == kGlobalArp) {
    if (pad >= kFirstArpModePad && pad < kFirstArpModePad + kNumArpModes) {
      return kArpModeLabels[pad - kFirstArpModePad];
    }
    if (pad >= kFirstArpRatePad && pad < kFirstArpRatePad + kNumArpRates) {
      return kArpRateLabels[pad - kFirstArpRatePad];
    }
    if (pad >= kFirstArpOctavePad && pad < kFirstArpOctavePad + kMaxArpOctaves) {
      return kArpOctaveLabels[pad - kFirstArpOctavePad];
    }
    return nullptr;
  }
  if (selected_ == kGlobalDevices) {
    if (pad == kRefreshPad) return "RE-\nFRESH";
    if (pad >= kFirstClockPad && pad - kFirstClockPad < kNumClockSources) {
      return kClockLabels[pad - kFirstClockPad];
    }
    if (pad >= kFirstDevicePad && pad - kFirstDevicePad < DeviceStatus::kNumStatusDevices) {
      return kDeviceLabels[pad - kFirstDevicePad];
    }
    if (pad >= kFirstPortPad && pad < kFirstPortPad + kNumMidiPorts) {
      return kPortLabels[pad - kFirstPortPad];
    }
    return nullptr;
  }
  if (isNumberSetting()) {
    if (pad == kCoarseUpPad) return "+10";
    if (pad == kCoarseDownPad) return "-10";
    if (pad == kUpPad) return "+1";
    if (pad == kDownPad) return "-1";
  }
  if (selected_ == kGlobalMidiChannel && pad >= kFirstChannelPad &&
      pad < kFirstChannelPad + kNumMidiChannels) {
    return kChannelLabels[pad - kFirstChannelPad];
  }
  if (selected_ == kGlobalMidiChannel && pad >= kFirstPortPad &&
      pad < kFirstPortPad + kNumMidiPorts) {
    return kPortLabels[pad - kFirstPortPad];
  }
  if (selected_ == kGlobalMidiChannel && pad >= kFirstInstrumentPad &&
      pad < kFirstInstrumentPad + kNumInstruments) {
    return kInstrumentLabels[pad - kFirstInstrumentPad];
  }
  return nullptr;
}

void GlobalMode::renderPads(const UiState& state, LedFrame& frame) const {
  for (uint8_t pad = 0; pad < kNumPads; ++pad) frame.pads[pad] = kBlack;
  for (uint8_t setting = 0; setting < kNumGlobalSettings; ++setting) {
    frame.pads[setting] = setting == selected_ ? kSelectedColor : dim(kWhite, kIdleSettingLevel);
  }
  frame.pads[kSendCcPad] = dim(kWhite, kSendCcLevel);
  if (selected_ == kGlobalDevices) {
    // Lit is here and answering; nearly dark is nothing on it. The ports keep the row they
    // have on the routing page, so P3 is the same pad on both.
    for (uint8_t device = 0; device < DeviceStatus::kNumStatusDevices; ++device) {
      const bool on = devices_ && devices_->deviceConnected(device);
      frame.pads[kFirstDevicePad + device] = on ? kWhite : dim(kWhite, kAbsentLevel);
    }
    for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
      const bool on = devices_ && devices_->portConnected(port);
      frame.pads[kFirstPortPad + port] = on ? kSelectedColor : dim(kSelectedColor, kAbsentLevel);
    }
    // Green while an outside clock is really driving us, white while our own is: on AUTO
    // that is the difference between "a clock is there" and "nothing is sending".
    const Rgb clockColor = sequencer_.followingExternal() ? kSelectedColor : kWhite;
    const uint8_t source = sequencer_.clockSource();
    for (uint8_t i = 0; i < kNumClockSources; ++i) {
      frame.pads[kFirstClockPad + i] =
          i == source ? clockColor : dim(clockColor, kIdleChannelLevel);
    }
    // Full white when something has appeared or gone: the pad is offering the rewire rather
    // than doing it, because a port changing under a take is the player's call.
    const bool waiting = devices_ && devices_->portsNeedRefresh();
    frame.pads[kRefreshPad] = dim(kWhite, waiting ? kRefreshWaitingLevel : kRefreshLevel);
    return;
  }
  if (selected_ == kGlobalArp) {
    // The track's colour for what is in use, dim for the rest: the same reading as its routing.
    const Rgb color = trackColor(state.track);
    const uint8_t mode = sequencer_.trackArpMode(state.track);
    for (uint8_t i = 0; i < kNumArpModes; ++i) {
      frame.pads[kFirstArpModePad + i] = i == mode ? color : dim(color, kIdleChannelLevel);
    }
    // The rate and the octaves say nothing while the arp is off, so they dim further.
    const uint8_t idle = mode == kArpOff ? kUnusedRouteLevel : kIdleChannelLevel;
    const uint8_t rate = sequencer_.trackArpRate(state.track);
    for (uint8_t i = 0; i < kNumArpRates; ++i) {
      frame.pads[kFirstArpRatePad + i] =
          (mode != kArpOff && i == rate) ? color : dim(color, idle);
    }
    const uint8_t octaves = sequencer_.trackArpOctaves(state.track);
    for (uint8_t i = 0; i < kMaxArpOctaves; ++i) {
      frame.pads[kFirstArpOctavePad + i] =
          (mode != kArpOff && i + 1 == octaves) ? color : dim(color, idle);
    }
    return;
  }
  if (selected_ == kGlobalMidiChannel) {
    const Rgb color = trackColor(state.track);
    const Rgb inUse = lift(color, kRouteInUseLift);
    const uint8_t current = sequencer_.trackMidiChannel(state.track);
    for (uint8_t channel = 0; channel < kNumMidiChannels; ++channel) {
      frame.pads[kFirstChannelPad + channel] =
          channel == current ? inUse : dim(color, kIdleChannelLevel);
    }
    // The track plays out of a MIDI port or on an internal instrument: the one in use lights
    // in the track colour, and the row it isn't using is dimmer still.
    const bool onInstrument = sequencer_.trackUsesInstrument(state.track);
    const uint8_t currentPort = sequencer_.trackMidiPort(state.track);
    const uint8_t idlePort = onInstrument ? kUnusedRouteLevel : kIdleChannelLevel;
    for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
      frame.pads[kFirstPortPad + port] =
          (!onInstrument && port == currentPort) ? inUse : dim(color, idlePort);
    }
    const uint8_t currentInstrument = sequencer_.trackInstrument(state.track);
    const uint8_t idleInstrument = onInstrument ? kIdleChannelLevel : kUnusedRouteLevel;
    for (uint8_t instrument = 0; instrument < kNumInstruments; ++instrument) {
      frame.pads[kFirstInstrumentPad + instrument] =
          instrument == currentInstrument ? inUse : dim(color, idleInstrument);
    }
    return;
  }
  if (!isNumberSetting()) return;
  const uint16_t current = value();
  drawNumber(frame, current);
  // Every stepper reads as unavailable while the tempo belongs to someone else, and dims at
  // the end it cannot move past.
  const bool locked = tempoIsExternal();
  for (uint8_t i = 0; i < kNumSteppers; ++i) {
    const Stepper& stepper = kSteppers[i];
    const bool atLimit = stepper.step > 0 ? current >= maxValue() : current <= minValue();
    frame.pads[stepper.pad] = stepPadColor(held_ & stepper.heldBit, locked || atLimit);
  }
}


// ---- what the rows are for, for a screen beside the instrument ----

namespace {

const Rgb kPickColor = {54, 214, 95};
const Rgb kValueColor = {40, 100, 255};
const Rgb kStepColor = {243, 244, 246};

const ModeLegend kGlobalLegend = {
    "SHIFT + R1",
    "Global settings",
    {
        {1, 1, kPickColor, "row 1", "tempo, MIDI, swing, devices, arp"},
        {3, 7, kValueColor, "rows 3-7", "the picked setting's value"},
        {8, 8, kStepColor, "row 8", "pad 5 +1, pad 6 -1, pad 8 resends CCs"},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
        {0, 0, {}, NULL, NULL},
    },
    3,
    {"fader 1 sets the picked value", "B1-B8 only move the selection here",
     "R1 back to project", NULL},
    3,
    {kPickColor, {}, kValueColor, kValueColor, kValueColor, kValueColor, kValueColor, kStepColor},
};

}  // namespace

const ModeLegend* GlobalMode::legend(const UiState&) const { return &kGlobalLegend; }

}  // namespace gx
