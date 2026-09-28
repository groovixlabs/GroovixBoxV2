#include "common/MidiMixSurface.h"

#include "ui/LedFrame.h"

namespace gx {

namespace {

// The factory map. Strips 1-4 start at CC 16 and strips 5-8 at CC 46, four controls each:
// three knobs then the fader.
const uint8_t kFirstCcLow = 16;
const uint8_t kFirstCcHigh = 46;
const uint8_t kControlsPerStrip = 4;
const uint8_t kStripsPerBlock = 4;
const uint8_t kMasterFaderCc = 62;

const uint8_t kFirstMuteNote = 1;   // MUTE n is 1 + 3n, REC ARM n is 3 + 3n
const uint8_t kRecArmOffset = 2;
const uint8_t kNotesPerStrip = 3;
const uint8_t kBankLeftNote = 25;
const uint8_t kBankRightNote = 26;
const uint8_t kSoloNote = 27;

const uint8_t kMidiNoteOn = 0x90;
const uint8_t kMidiNoteOff = 0x80;
const uint8_t kMidiControlChange = 0xB0;
const uint8_t kMaxMidiData = 127;

// The strip and the control within it (0..2 knobs, 3 fader), or false if the CC isn't ours.
bool stripForCc(uint8_t cc, uint8_t& strip, uint8_t& control) {
  uint8_t first = 0;
  uint8_t block = 0;
  if (cc >= kFirstCcLow && cc < kFirstCcLow + kStripsPerBlock * kControlsPerStrip) {
    first = kFirstCcLow;
  } else if (cc >= kFirstCcHigh && cc < kFirstCcHigh + kStripsPerBlock * kControlsPerStrip) {
    first = kFirstCcHigh;
    block = kStripsPerBlock;
  } else {
    return false;
  }
  const uint8_t offset = static_cast<uint8_t>(cc - first);
  strip = static_cast<uint8_t>(block + offset / kControlsPerStrip);
  control = static_cast<uint8_t>(offset % kControlsPerStrip);
  return true;
}

// A 7-bit MIDI value as a fader position.
uint16_t faderPosition(uint8_t value) {
  return static_cast<uint16_t>(value * kFaderMax / kMaxMidiData);
}

// The note that lights a mixer button's LED: the A1 row is MUTE, the A2 row REC ARM.
uint8_t noteForMixButton(uint8_t index) {
  const uint8_t row = static_cast<uint8_t>(index / kNumMixStrips);
  const uint8_t strip = static_cast<uint8_t>(index % kNumMixStrips);
  return static_cast<uint8_t>(kFirstMuteNote + strip * kNotesPerStrip +
                              (row == 1 ? kRecArmOffset : 0));
}

// The panel's LEDs are single-colour, so a colour is either on or off. The UI dims a button
// rather than turning it off — a track silenced by someone else's solo sits at 40 of 255 —
// so anything below this counts as off and only a track's own state lights the button.
const uint8_t kOnThreshold = 64;

bool litEnough(const Rgb& color) {
  const uint8_t brightest = color.r > color.g ? (color.r > color.b ? color.r : color.b)
                                              : (color.g > color.b ? color.g : color.b);
  return brightest >= kOnThreshold;
}

}  // namespace

MidiMixSurface::MidiMixSurface(MidiPort& port)
    : port_(port),
      queueHead_(0),
      queueCount_(0),
      runningStatus_(0),
      dataCount_(0),
      inSysEx_(false),
      ledsKnown_(false),
      bankStep_(0) {
  for (uint8_t i = 0; i < sizeof(ledState_); ++i) ledState_[i] = 0;
}

MidiMixSurface::~MidiMixSurface() { clearLeds(); }

bool MidiMixSurface::pollEvent(ControlEvent& event) {
  uint8_t buffer[32];
  size_t read = port_.read(buffer, sizeof(buffer));
  while (read > 0) {
    for (size_t i = 0; i < read; ++i) parse(buffer[i]);
    read = port_.read(buffer, sizeof(buffer));
  }
  if (queueCount_ == 0) return false;
  event = queue_[queueHead_];
  queueHead_ = static_cast<uint8_t>((queueHead_ + 1) % kQueueSize);
  --queueCount_;
  return true;
}

bool MidiMixSurface::takeBankStep(int8_t& step) {
  if (bankStep_ == 0) return false;
  step = bankStep_;
  bankStep_ = 0;
  return true;
}

void MidiMixSurface::parse(uint8_t byte) {
  if (byte >= 0xF8) return;  // realtime bytes can arrive inside a message
  if (byte == 0xF0) {
    inSysEx_ = true;
    return;
  }
  if (byte == 0xF7) {
    inSysEx_ = false;
    return;
  }
  if (inSysEx_) return;  // the Mix sends none, but a device query would land here
  if (byte & 0x80) {
    runningStatus_ = byte;
    dataCount_ = 0;
    return;
  }
  if (runningStatus_ == 0) return;
  data_[dataCount_++] = byte;
  if (dataCount_ < 2) return;
  dataCount_ = 0;
  handleMessage(runningStatus_, data_[0], data_[1]);
}

void MidiMixSurface::handleMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  const uint8_t type = static_cast<uint8_t>(status & 0xF0);
  if (type == kMidiControlChange) {
    uint8_t strip = 0;
    uint8_t control = 0;
    if (data1 == kMasterFaderCc) {
      push(kGroupMixMaster, 0, false, faderPosition(data2));
    } else if (stripForCc(data1, strip, control)) {
      if (control < kKnobRows) {
        push(kGroupKnob, knobIndex(control, strip), false, faderPosition(data2));
      } else {
        push(kGroupMixFader, strip, false, faderPosition(data2));
      }
    }
    return;
  }
  if (type != kMidiNoteOn && type != kMidiNoteOff) return;
  const bool pressed = type == kMidiNoteOn && data2 > 0;

  if (data1 == kSoloNote) {
    push(kGroupMixSide, kMixShiftButton, pressed, 0);  // SOLO is the mixer's Shift
    // While SOLO is held the panel lights its own MUTE row, to say those buttons solo now.
    // It hands the LEDs back on release, so the whole state goes out again rather than
    // trusting what is up there.
    if (!pressed) ledsKnown_ = false;
    return;
  }
  if (data1 == kBankLeftNote || data1 == kBankRightNote) {
    // The banks move the track page, which the app applies: the strips and the pads follow.
    if (pressed) bankStep_ = data1 == kBankLeftNote ? -1 : 1;
    return;
  }
  if (data1 < kFirstMuteNote) return;
  const uint8_t offset = static_cast<uint8_t>(data1 - kFirstMuteNote);
  const uint8_t strip = static_cast<uint8_t>(offset / kNotesPerStrip);
  const uint8_t which = static_cast<uint8_t>(offset % kNotesPerStrip);
  if (strip >= kNumMixStrips) return;
  // Each strip owns three notes: MUTE, SOLO and REC ARM. The panel switches the mute buttons
  // to their solo notes while SOLO is held, so both are the A1 row — the app is holding the
  // mixer's Shift by then, and solos rather than mutes.
  const uint8_t row = which == kRecArmOffset ? 1 : 0;
  push(kGroupMixButton, mixButtonIndex(row, strip), pressed, 0);
}

void MidiMixSurface::push(uint8_t group, uint8_t index, bool pressed, uint16_t value) {
  if (queueCount_ >= kQueueSize) return;  // the app is not draining: drop rather than block
  const uint8_t slot = static_cast<uint8_t>((queueHead_ + queueCount_) % kQueueSize);
  queue_[slot].group = group;
  queue_[slot].index = index;
  queue_[slot].pressed = pressed;
  queue_[slot].value = value;
  queue_[slot].initial = false;  // it cannot say where its controls already are
  queue_[slot].velocity = 0;     // and it has no pads to hit
  ++queueCount_;
}

void MidiMixSurface::sendLed(uint8_t note, bool on) {
  // Always Note On: the panel ignores Note Off, and only velocity 0 puts an LED out. Checked
  // on the device — a Note Off leaves it lit.
  const uint8_t message[3] = {kMidiNoteOn, note, static_cast<uint8_t>(on ? kMaxMidiData : 0)};
  port_.write(message, sizeof(message));
}

void MidiMixSurface::show(const LedFrame& frame) {
  // Single-colour LEDs: a lit pad colour turns one on, anything dim turns it off. Only
  // changes go out, so an idle panel sends nothing.
  for (uint8_t i = 0; i < kNumMixButtons; ++i) {
    const uint8_t lit = litEnough(frame.mixButtons[i]) ? 1 : 0;
    if (ledsKnown_ && ledState_[i] == lit) continue;
    ledState_[i] = lit;
    sendLed(noteForMixButton(i), lit != 0);
  }
  // The side column: only the mixer's Shift, SOLO, has an LED of ours to show.
  const uint8_t soloIndex = static_cast<uint8_t>(kNumMixButtons + kMixShiftButton);
  const uint8_t soloLit = litEnough(frame.mixSide[kMixShiftButton]) ? 1 : 0;
  if (!ledsKnown_ || ledState_[soloIndex] != soloLit) {
    ledState_[soloIndex] = soloLit;
    sendLed(kSoloNote, soloLit != 0);
  }
  ledsKnown_ = true;
}

void MidiMixSurface::clearLeds() {
  for (uint8_t strip = 0; strip < kNumMixStrips; ++strip) {
    sendLed(static_cast<uint8_t>(kFirstMuteNote + strip * kNotesPerStrip), false);
    sendLed(static_cast<uint8_t>(kFirstMuteNote + strip * kNotesPerStrip + kRecArmOffset), false);
  }
  sendLed(kSoloNote, false);
  sendLed(kBankLeftNote, false);
  sendLed(kBankRightNote, false);
  for (uint8_t i = 0; i < sizeof(ledState_); ++i) ledState_[i] = 0;
  ledsKnown_ = true;
}

}  // namespace gx
