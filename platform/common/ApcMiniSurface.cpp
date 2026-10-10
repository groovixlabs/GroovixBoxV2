#include "common/ApcMiniSurface.h"

namespace gx {

namespace {

// Values from the APC mini mk2 Communications Protocol v1.0.
const uint8_t kNumPadNotes = 64;       // notes 0..63, 0 = bottom-left pad
const uint8_t kFirstTrackNote = 0x64;  // buttons under the grid: our B1..B8
const uint8_t kFirstSceneNote = 0x70;  // buttons right of the grid, top down: our R1..R8
const uint8_t kShiftNote = 0x7A;
const uint8_t kFirstFaderCc = 0x30;  // faders 1..8; the master fader follows at 0x38
const uint8_t kMaxMidiValue7 = 127;

const uint8_t kNoteOff = 0x80;
const uint8_t kNoteOn = 0x90;
const uint8_t kControlChange = 0xB0;
const uint8_t kSysExStart = 0xF0;
const uint8_t kSysExEnd = 0xF7;
const uint8_t kFirstRealTime = 0xF8;  // clock and friends, which may arrive mid-message

const uint8_t kAkaiId = 0x47;
const uint8_t kDeviceId = 0x7F;
const uint8_t kModelId = 0x4F;
const uint8_t kIntroductionMessage = 0x60;
const uint8_t kIntroductionReply = 0x61;
const uint8_t kReplyFaderOffset = 6;  // after 47 7F 4F 61 and the two length bytes

// Button LEDs are one colour: on for a bright UI colour, off for the dim ones.
const uint8_t kButtonOnLevel = 200;
const uint8_t kButtonLedOff = 0x00;
const uint8_t kButtonLedOn = 0x01;

// At most this many pads change per frame. Bursts get lost if it is too high; with palette
// messages at 3 bytes a pad this is 72 bytes a frame, against 71 for 8 pads of RGB SysEx.
const uint8_t kPadsPerShow = 24;
const uint8_t kIntroWaitFrames = 30;   // about half a second of frames without an answer

// Our pad index (row 0 at the top) as the device's note (row 0 at the bottom), and back.
uint8_t noteForPad(uint8_t pad) {
  const uint8_t rowFromTop = pad / kGridCols;
  return static_cast<uint8_t>((kGridRows - 1 - rowFromTop) * kGridCols + pad % kGridCols);
}

uint8_t padForNote(uint8_t note) {
  const uint8_t rowFromBottom = note / kGridCols;
  return padIndex(static_cast<uint8_t>(kGridRows - 1 - rowFromBottom), note % kGridCols);
}

uint16_t faderValue(uint8_t midiValue) {
  if (midiValue > kMaxMidiValue7) midiValue = kMaxMidiValue7;
  return static_cast<uint16_t>(midiValue * kFaderMax / kMaxMidiValue7);
}

bool buttonLit(Rgb c) {
  return c.r >= kButtonOnLevel || c.g >= kButtonOnLevel || c.b >= kButtonOnLevel;
}


uint64_t padBit(uint8_t pad) { return static_cast<uint64_t>(1) << pad; }

// The colour a pad is sent, so that the UI's dim colours are still visible on the hardware.
// The device's fixed 128-colour palette, from the protocol document: the velocity of a note
// picks one of these, and the channel picks how brightly it is lit. Only the hue matters here,
// since the brightness comes from the channel, so entries that differ only in brightness are
// interchangeable.
const Rgb kPalette[128] = {
    {0x00, 0x00, 0x00}, {0x1E, 0x1E, 0x1E}, {0x7F, 0x7F, 0x7F}, {0xFF, 0xFF, 0xFF},
    {0xFF, 0x4C, 0x4C}, {0xFF, 0x00, 0x00}, {0x59, 0x00, 0x00}, {0x19, 0x00, 0x00},
    {0xFF, 0xBD, 0x6C}, {0xFF, 0x54, 0x00}, {0x59, 0x1D, 0x00}, {0x27, 0x1B, 0x00},
    {0xFF, 0xFF, 0x4C}, {0xFF, 0xFF, 0x00}, {0x59, 0x59, 0x00}, {0x19, 0x19, 0x00},
    {0x88, 0xFF, 0x4C}, {0x54, 0xFF, 0x00}, {0x1D, 0x59, 0x00}, {0x14, 0x2B, 0x00},
    {0x4C, 0xFF, 0x4C}, {0x00, 0xFF, 0x00}, {0x00, 0x59, 0x00}, {0x00, 0x19, 0x00},
    {0x4C, 0xFF, 0x5E}, {0x00, 0xFF, 0x19}, {0x00, 0x59, 0x0D}, {0x00, 0x19, 0x02},
    {0x4C, 0xFF, 0x88}, {0x00, 0xFF, 0x55}, {0x00, 0x59, 0x1D}, {0x00, 0x1F, 0x12},
    {0x4C, 0xFF, 0xB7}, {0x00, 0xFF, 0x99}, {0x00, 0x59, 0x35}, {0x00, 0x19, 0x12},
    {0x4C, 0xC3, 0xFF}, {0x00, 0xA9, 0xFF}, {0x00, 0x41, 0x52}, {0x00, 0x10, 0x19},
    {0x4C, 0x88, 0xFF}, {0x00, 0x55, 0xFF}, {0x00, 0x1D, 0x59}, {0x00, 0x08, 0x19},
    {0x4C, 0x4C, 0xFF}, {0x00, 0x00, 0xFF}, {0x00, 0x00, 0x59}, {0x00, 0x00, 0x19},
    {0x87, 0x4C, 0xFF}, {0x54, 0x00, 0xFF}, {0x19, 0x00, 0x64}, {0x0F, 0x00, 0x30},
    {0xFF, 0x4C, 0xFF}, {0xFF, 0x00, 0xFF}, {0x59, 0x00, 0x59}, {0x19, 0x00, 0x19},
    {0xFF, 0x4C, 0x87}, {0xFF, 0x00, 0x54}, {0x59, 0x00, 0x1D}, {0x22, 0x00, 0x13},
    {0xFF, 0x15, 0x00}, {0x99, 0x35, 0x00}, {0x79, 0x51, 0x00}, {0x43, 0x64, 0x00},
    {0x03, 0x39, 0x00}, {0x00, 0x57, 0x35}, {0x00, 0x54, 0x7F}, {0x00, 0x00, 0xFF},
    {0x00, 0x45, 0x4F}, {0x25, 0x00, 0xCC}, {0x7F, 0x7F, 0x7F}, {0x20, 0x20, 0x20},
    {0xFF, 0x00, 0x00}, {0xBD, 0xFF, 0x2D}, {0xAF, 0xED, 0x06}, {0x64, 0xFF, 0x09},
    {0x10, 0x8B, 0x00}, {0x00, 0xFF, 0x87}, {0x00, 0xA9, 0xFF}, {0x00, 0x2A, 0xFF},
    {0x3F, 0x00, 0xFF}, {0x7A, 0x00, 0xFF}, {0xB2, 0x1A, 0x7D}, {0x40, 0x21, 0x00},
    {0xFF, 0x4A, 0x00}, {0x88, 0xE1, 0x06}, {0x72, 0xFF, 0x15}, {0x00, 0xFF, 0x00},
    {0x3B, 0xFF, 0x26}, {0x59, 0xFF, 0x71}, {0x38, 0xFF, 0xCC}, {0x5B, 0x8A, 0xFF},
    {0x31, 0x51, 0xC6}, {0x87, 0x7F, 0xE9}, {0xD3, 0x1D, 0xFF}, {0xFF, 0x00, 0x5D},
    {0xFF, 0x7F, 0x00}, {0xB9, 0xB0, 0x00}, {0x90, 0xFF, 0x00}, {0x83, 0x5D, 0x07},
    {0x39, 0x2B, 0x00}, {0x14, 0x4C, 0x10}, {0x0D, 0x50, 0x38}, {0x15, 0x15, 0x2A},
    {0x16, 0x20, 0x5A}, {0x69, 0x3C, 0x1C}, {0xA8, 0x00, 0x0A}, {0xDE, 0x51, 0x3D},
    {0xD8, 0x6A, 0x1C}, {0xFF, 0xE1, 0x26}, {0x9E, 0xE1, 0x2F}, {0x67, 0xB5, 0x0F},
    {0x1E, 0x1E, 0x30}, {0xDC, 0xFF, 0x6B}, {0x80, 0xFF, 0xBD}, {0x9A, 0x99, 0xFF},
    {0x8E, 0x66, 0xFF}, {0x40, 0x40, 0x40}, {0x75, 0x75, 0x75}, {0xE0, 0xFF, 0xFF},
    {0xA0, 0x00, 0x00}, {0x35, 0x00, 0x00}, {0x1A, 0xD0, 0x00}, {0x07, 0x42, 0x00},
    {0xB9, 0xB0, 0x00}, {0x3F, 0x31, 0x00}, {0xB3, 0x5F, 0x00}, {0x4B, 0x15, 0x02}
};

// The pads read as barely there below about a fifth of full, and the UI leans on very dim
// shades — an empty step is 14 of 255. Lit colours are lifted into kMinPadLevel..255, which
// keeps their order and their hue: the dimmest states end up about as bright as the UI's
// idle white, which is what they looked like before the palette.
const uint8_t kMinPadLevel = 50;

Rgb liftToVisible(Rgb c) {
  unsigned brightest = c.r;
  if (c.g > brightest) brightest = c.g;
  if (c.b > brightest) brightest = c.b;
  if (brightest == 0) return c;  // black stays off
  const unsigned target = kMinPadLevel + brightest * (255u - kMinPadLevel) / 255u;
  const Rgb lifted = {static_cast<uint8_t>(c.r * target / brightest),
                      static_cast<uint8_t>(c.g * target / brightest),
                      static_cast<uint8_t>(c.b * target / brightest)};
  return lifted;
}

// Channels 0..6 light a pad at these percentages; 7 and above pulse or blink, which the UI
// does not use.
const uint8_t kSolidChannels = 7;
const uint8_t kChannelBrightness[kSolidChannels] = {10, 25, 50, 65, 75, 90, 100};

// Squared distance between a wanted colour and what a palette entry lit at a brightness
// actually looks like. Matching the hue alone is not enough: the UI's idle states are very
// dim — an empty step is about 5% — and the lowest channel is 10% of a full palette colour,
// so hue-only matching lit the whole grid.
uint32_t distanceTo(Rgb want, Rgb entry, uint8_t percent) {
  const int r = entry.r * percent / 100, g = entry.g * percent / 100, b = entry.b * percent / 100;
  const int dr = want.r - r, dg = want.g - g, db = want.b - b;
  return static_cast<uint32_t>(dr * dr + dg * dg + db * db);
}

uint8_t brightestChannel(Rgb c) {
  uint8_t brightest = c.r;
  if (c.g > brightest) brightest = c.g;
  if (c.b > brightest) brightest = c.b;
  return brightest;
}

// A colour as the device wants it: the note's channel (brightness) and velocity (palette
// entry). Black is velocity 0, which is off whatever the channel.
void padCode(Rgb wanted, uint8_t& channel, uint8_t& velocity) {
  const Rgb color = liftToVisible(wanted);
  const uint8_t brightest = brightestChannel(color);
  if (brightest == 0) {
    channel = 0;
    velocity = 0;
    return;
  }
  // Every colour the device can show is a palette entry at one of seven brightnesses, so
  // take the nearest of those: it keeps the hue where the colour is bright and follows the
  // UI down to its faintest shades, which is what the idle states depend on.
  uint32_t bestDistance = 0xFFFFFFFF;
  channel = kSolidChannels - 1;
  velocity = 1;
  for (uint8_t entry = 1; entry < 128; ++entry) {
    if (brightestChannel(kPalette[entry]) == 0) continue;
    for (uint8_t c = 0; c < kSolidChannels; ++c) {
      const uint32_t distance = distanceTo(color, kPalette[entry], kChannelBrightness[c]);
      if (distance < bestDistance) {
        bestDistance = distance;
        channel = c;
        velocity = entry;
      }
    }
  }
}

}  // namespace

static_assert(kNumPadNotes == kNumPads && kNumPads <= 64, "one note, and one bit, per pad");

ApcMiniSurface::ApcMiniSurface(MidiPort& port)
    : port_(port),
      queueHead_(0),
      queueCount_(0),
      runningStatus_(0),
      dataCount_(0),
      inSysEx_(false),
      sysExLength_(0),
      cacheNext_(0),
      cacheUsed_(0),
      padsKnown_(0),
      buttonsKnown_(false),
      nextPad_(0),
      ready_(false),
      framesWaited_(0) {
  sent_.clear();
}

ApcMiniSurface::~ApcMiniSurface() { clearLeds(); }

bool ApcMiniSurface::begin() {
  // Application 0, version 1.0.0. The reply carries all nine fader positions.
  const uint8_t intro[] = {kSysExStart, kAkaiId, kDeviceId, kModelId, kIntroductionMessage,
                           0x00,        0x04,    0x00,      0x01,     0x00,
                           0x00,        kSysExEnd};
  return port_.write(intro, sizeof(intro));
}

bool ApcMiniSurface::pollEvent(ControlEvent& event) {
  uint8_t buffer[64];
  while (queueCount_ == 0) {
    const size_t count = port_.read(buffer, sizeof(buffer));
    if (count == 0) return false;
    for (size_t i = 0; i < count; ++i) parse(buffer[i]);
  }
  event = queue_[queueHead_];
  queueHead_ = static_cast<uint8_t>((queueHead_ + 1) % kQueueSize);
  --queueCount_;
  return true;
}

void ApcMiniSurface::show(const LedFrame& frame) {
  if (!ready_) {
    if (framesWaited_ < kIntroWaitFrames) {
      ++framesWaited_;
      return;
    }
    ready_ = true;  // no answer; carry on regardless
  }
  sendButtons(frame);
  sendPads(frame);
}

void ApcMiniSurface::clearLeds() {
  // Velocity 0 is off whatever the channel, so the whole grid is 64 short messages.
  uint8_t message[kNumPads * 3];
  size_t size = 0;
  for (uint8_t pad = 0; pad < kNumPads; ++pad) {
    message[size++] = kNoteOn;
    message[size++] = noteForPad(pad);
    message[size++] = 0;
  }
  port_.write(message, size);

  LedFrame dark;
  dark.clear();
  buttonsKnown_ = false;
  sendButtons(dark);
  sent_ = dark;
  padsKnown_ = ~static_cast<uint64_t>(0);
}

void ApcMiniSurface::parse(uint8_t byte) {
  if (byte >= kFirstRealTime) return;
  if (byte == kSysExStart) {
    inSysEx_ = true;
    sysExLength_ = 0;
    return;
  }
  if (inSysEx_) {
    if (byte == kSysExEnd) {
      inSysEx_ = false;
      handleSysEx();
      return;
    }
    if (!(byte & 0x80)) {
      if (sysExLength_ < kMaxSysEx) sysEx_[sysExLength_++] = byte;
      return;
    }
    inSysEx_ = false;  // cut off by a new status byte, which is handled below
  }
  if (byte & 0x80) {
    runningStatus_ = byte < kSysExStart ? byte : 0;  // system messages end running status
    dataCount_ = 0;
    return;
  }
  if (runningStatus_ == 0) return;  // data with no status

  data_[dataCount_++] = byte;
  const uint8_t type = runningStatus_ & 0xF0;
  const uint8_t needed = (type == 0xC0 || type == 0xD0) ? 1 : 2;
  if (dataCount_ < needed) return;
  dataCount_ = 0;
  handleMessage(runningStatus_, data_[0], needed == 2 ? data_[1] : 0);
}

void ApcMiniSurface::handleMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  // The device's own Drum and Note modes move the pads to other channels; only its
  // default mode, on channel 1, drives the sequencer.
  if ((status & 0x0F) != 0) return;
  const uint8_t type = status & 0xF0;
  if (type == kNoteOn || type == kNoteOff) {
    const bool pressed = type == kNoteOn && data2 > 0;
    if (data1 < kNumPadNotes) {
      // The mk2's pads are velocity sensitive, and that is what a recorded note is worth.
      push(kGroupPad, padForNote(data1), pressed, 0, false, pressed ? data2 : 0);
    } else if (data1 >= kFirstTrackNote && data1 < kFirstTrackNote + kNumBottomButtons) {
      push(kGroupBottom, static_cast<uint8_t>(data1 - kFirstTrackNote), pressed, 0);
    } else if (data1 >= kFirstSceneNote && data1 < kFirstSceneNote + kNumRightButtons) {
      push(kGroupRight, static_cast<uint8_t>(data1 - kFirstSceneNote), pressed, 0);
    } else if (data1 == kShiftNote) {
      push(kGroupShift, 0, pressed, 0);
    }
  } else if (type == kControlChange) {
    if (data1 >= kFirstFaderCc && data1 < kFirstFaderCc + kNumTrackFaders) {
      push(kGroupFader, static_cast<uint8_t>(data1 - kFirstFaderCc), false, faderValue(data2));
    } else if (data1 == kFirstFaderCc + kNumTrackFaders) {
      push(kGroupMasterFader, 0, false, faderValue(data2));
    }
  }
}

void ApcMiniSurface::handleSysEx() {
  // The Introduction reply: 47 7F 4F 61 <length MSB> <length LSB> <faders 1..8> <master>.
  if (sysExLength_ < kReplyFaderOffset + kNumTrackFaders + 1 || sysEx_[0] != kAkaiId ||
      sysEx_[2] != kModelId || sysEx_[3] != kIntroductionReply) {
    return;
  }
  ready_ = true;
  for (uint8_t i = 0; i < kNumTrackFaders; ++i) {
    push(kGroupFader, i, false, faderValue(sysEx_[kReplyFaderOffset + i]), true);
  }
  push(kGroupMasterFader, 0, false, faderValue(sysEx_[kReplyFaderOffset + kNumTrackFaders]),
       true);
}

void ApcMiniSurface::push(uint8_t group, uint8_t index, bool pressed, uint16_t value,
                          bool initial, uint8_t velocity) {
  if (queueCount_ == kQueueSize) return;  // far behind: drop rather than block
  ControlEvent& e = queue_[(queueHead_ + queueCount_) % kQueueSize];
  e.group = group;
  e.index = index;
  e.pressed = pressed;
  e.value = value;
  e.initial = initial;
  e.velocity = velocity;
  ++queueCount_;
}

// The device's colour for a message, which is what the search above matches against.
Rgb ApcMiniSurface::ledColor(uint8_t channel, uint8_t velocity) {
  if (velocity >= 128 || channel >= kSolidChannels) return kBlack;
  const Rgb entry = kPalette[velocity];
  const uint8_t percent = kChannelBrightness[channel];
  const Rgb shown = {static_cast<uint8_t>(entry.r * percent / 100),
                     static_cast<uint8_t>(entry.g * percent / 100),
                     static_cast<uint8_t>(entry.b * percent / 100)};
  return shown;
}

// The message for a colour. Static, so the panel surface - the same hardware read another
// way - lights a colour exactly as the grid does.
uint16_t ApcMiniSurface::padCodeFor(Rgb color) {
  uint8_t channel = 0;
  uint8_t velocity = 0;
  padCode(color, channel, velocity);
  return static_cast<uint16_t>((channel << 8) | velocity);
}

// Encodes a colour, remembering the last few: a grid is mostly a handful of shades, so the
// palette search runs a couple of dozen times rather than once per pad.
uint16_t ApcMiniSurface::codeFor(Rgb color) {
  const uint32_t key = (static_cast<uint32_t>(color.r) << 16) |
                       (static_cast<uint32_t>(color.g) << 8) | color.b;
  for (uint8_t i = 0; i < cacheUsed_; ++i) {
    if (cacheKey_[i] == key) return cacheCode_[i];
  }
  const uint16_t code = padCodeFor(color);
  cacheKey_[cacheNext_] = key;
  cacheCode_[cacheNext_] = code;
  cacheNext_ = static_cast<uint8_t>((cacheNext_ + 1) % kCacheSize);
  if (cacheUsed_ < kCacheSize) ++cacheUsed_;
  return code;
}

void ApcMiniSurface::sendPads(const LedFrame& frame) {
  uint8_t message[kPadsPerShow * 3];
  size_t size = 0;
  uint8_t sent = 0;
  uint8_t lastPad = 0;
  // Start where the last frame stopped, so every changed pad gets its turn.
  for (uint8_t n = 0; n < kNumPads && sent < kPadsPerShow; ++n) {
    const uint8_t pad = static_cast<uint8_t>((nextPad_ + n) % kNumPads);
    const uint16_t code = codeFor(frame.pads[pad]);
    const uint8_t channel = static_cast<uint8_t>(code >> 8);
    const uint8_t velocity = static_cast<uint8_t>(code & 0xFF);
    if ((padsKnown_ & padBit(pad)) && padCode_[pad] == code) continue;
    message[size++] = static_cast<uint8_t>(kNoteOn | channel);
    message[size++] = noteForPad(pad);
    message[size++] = velocity;
    padCode_[pad] = code;
    padsKnown_ |= padBit(pad);
    lastPad = pad;
    ++sent;
  }
  if (sent == 0) return;
  nextPad_ = static_cast<uint8_t>((lastPad + 1) % kNumPads);
  port_.write(message, size);
}

void ApcMiniSurface::sendButtons(const LedFrame& frame) {
  uint8_t message[3 * (kNumRightButtons + kNumBottomButtons)];
  size_t size = 0;
  for (uint8_t i = 0; i < kNumRightButtons; ++i) {
    const bool lit = buttonLit(frame.right[i]);
    if (buttonsKnown_ && lit == buttonLit(sent_.right[i])) continue;
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstSceneNote + i);
    message[size++] = lit ? kButtonLedOn : kButtonLedOff;
    sent_.right[i] = frame.right[i];
  }
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) {
    const bool lit = buttonLit(frame.bottom[i]);
    if (buttonsKnown_ && lit == buttonLit(sent_.bottom[i])) continue;
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstTrackNote + i);
    message[size++] = lit ? kButtonLedOn : kButtonLedOff;
    sent_.bottom[i] = frame.bottom[i];
  }
  buttonsKnown_ = true;
  if (size > 0) port_.write(message, size);
}

}  // namespace gx
