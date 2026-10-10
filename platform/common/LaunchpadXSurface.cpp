#include "common/LaunchpadXSurface.h"

#include <string.h>

namespace gx {

namespace {

const uint8_t kNoteOff = 0x80;
const uint8_t kNoteOn = 0x90;
const uint8_t kControlChange = 0xB0;
const uint8_t kSysExStart = 0xF0;
const uint8_t kSysExEnd = 0xF7;
const uint8_t kFirstRealTime = 0xF8;

// Novation, Launchpad X. Every message of ours starts with these six bytes.
const uint8_t kHeader[] = {kSysExStart, 0x00, 0x20, 0x29, 0x02, 0x0C};
const uint8_t kModeMessage = 0x0E;  // 00 Live, 01 Programmer
const uint8_t kLedMessage = 0x03;
const uint8_t kRgbSpec = 0x03;      // one LED, as three 7-bit channels

const uint8_t kMaxMidi7 = 127;
const uint8_t kPressedValue = 127;  // what a round button sends; 0 on release

// The grid is rows 1..8 and columns 1..8 of the device's row*10+column numbering, with row 1
// at the BOTTOM. Our pads number row 0 at the top, so the row is flipped and the column is
// not - the same relationship the APC's grid has, in different arithmetic.
const uint8_t kRowStride = 10;
const uint8_t kButtonColumn = 9;  // the round buttons right of the grid
const uint8_t kButtonRow = 9;     // the round buttons above it

uint8_t padForNote(uint8_t note) {
  const uint8_t row = static_cast<uint8_t>(note / kRowStride);
  const uint8_t column = static_cast<uint8_t>(note % kRowStride);
  // kNumPads for anything that is not a grid pad: the round buttons are CCs, but the device
  // also numbers gaps (10, 20, ...) that nothing at all sits on.
  if (row < 1 || row > kGridRows || column < 1 || column > kGridCols) return kNumPads;
  return padIndex(static_cast<uint8_t>(kGridRows - row), static_cast<uint8_t>(column - 1));
}

// Column 9 of rows 8 down to 1: 89, 79 ... 19.
bool rightButtonForCc(uint8_t cc, uint8_t& button) {
  if (cc % kRowStride != kButtonColumn) return false;
  const uint8_t row = static_cast<uint8_t>(cc / kRowStride);
  if (row < 1 || row > kNumRightButtons) return false;
  button = static_cast<uint8_t>(kNumRightButtons - row);
  return true;
}

// Row 9, columns 1 to 8: 91..98.
bool bottomButtonForCc(uint8_t cc, uint8_t& button) {
  if (cc / kRowStride != kButtonRow) return false;
  const uint8_t column = static_cast<uint8_t>(cc % kRowStride);
  if (column < 1 || column > kNumBottomButtons) return false;
  button = static_cast<uint8_t>(column - 1);
  return true;
}

bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// The device takes 7 bits a channel where the UI has 8.
uint8_t channel7(uint8_t value) { return static_cast<uint8_t>(value >> 1); }

}  // namespace

static_assert(kGridRows == 8 && kGridCols == 8, "the Launchpad X grid is eight by eight");
static_assert(kNumRightButtons == 8 && kNumBottomButtons == 8,
              "sixteen edge buttons is exactly R1..R8 and B1..B8, with none to spare");

uint8_t LaunchpadXSurface::noteForPad(uint8_t pad) {
  const uint8_t rowFromTop = static_cast<uint8_t>(pad / kGridCols);
  const uint8_t column = static_cast<uint8_t>(pad % kGridCols);
  return static_cast<uint8_t>((kGridRows - rowFromTop) * kRowStride + column + 1);
}

uint8_t LaunchpadXSurface::ccForRightButton(uint8_t button) {
  return static_cast<uint8_t>((kNumRightButtons - button) * kRowStride + kButtonColumn);
}

uint8_t LaunchpadXSurface::ccForBottomButton(uint8_t button) {
  return static_cast<uint8_t>(kButtonRow * kRowStride + button + 1);
}

LaunchpadXSurface::LaunchpadXSurface(MidiPort& port)
    : port_(port),
      queueHead_(0),
      queueCount_(0),
      runningStatus_(0),
      dataCount_(0),
      inSysEx_(false),
      ledsKnown_(false),
      nextPad_(0) {
  memset(padShown_, 0, sizeof(padShown_));
  memset(rightShown_, 0, sizeof(rightShown_));
  memset(bottomShown_, 0, sizeof(bottomShown_));
}

// Back to Live mode on the way out, so the next piece of software to open the device finds it
// the way it expects rather than in a mode we put it in.
LaunchpadXSurface::~LaunchpadXSurface() {
  clearLeds();
  setMode(false);
}

void LaunchpadXSurface::setMode(bool programmer) {
  uint8_t message[sizeof(kHeader) + 3];
  size_t at = 0;
  memcpy(message, kHeader, sizeof(kHeader));
  at = sizeof(kHeader);
  message[at++] = kModeMessage;
  message[at++] = programmer ? 1 : 0;
  message[at++] = kSysExEnd;
  port_.write(message, at);
}

// Programmer mode, or the firmware keeps the grid for its own Session, Note and Custom modes
// and never passes those presses on.
bool LaunchpadXSurface::begin() {
  setMode(true);
  clearLeds();
  return true;
}

bool LaunchpadXSurface::pollEvent(ControlEvent& event) {
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

void LaunchpadXSurface::clearLeds() {
  LedFrame dark;
  dark.clear();
  ledsKnown_ = false;  // say nothing is known, so every LED is written
  // Darkening is the one frame that must go out whole rather than paced: it happens when
  // nothing is waiting on the device, and leaving half the grid lit would be worse than the
  // burst. kNumPads + the buttons is 80 LEDs, 407 bytes.
  const uint8_t saved = nextPad_;
  for (uint8_t round = 0; round < (kNumPads + kLedsPerShow - 1) / kLedsPerShow + 1; ++round) {
    show(dark);
  }
  nextPad_ = saved;
}

void LaunchpadXSurface::parse(uint8_t byte) {
  if (byte >= kFirstRealTime) return;
  if (byte == kSysExStart) {
    inSysEx_ = true;
    return;
  }
  if (inSysEx_) {
    // The device echoes a mode change back at us and has nothing else to say, so SysEx is
    // swallowed whole rather than collected.
    if (byte == kSysExEnd) {
      inSysEx_ = false;
      return;
    }
    if (!(byte & 0x80)) return;
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

void LaunchpadXSurface::handleMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  if ((status & 0x0F) != 0) return;  // the surface speaks on channel 1
  const uint8_t type = status & 0xF0;
  // Notes and CCs are the whole surface, and acting on nothing else is what drops the
  // POLYPHONIC AFTERTOUCH the pads bury the port in: four or five messages for a tap and a
  // steady stream while a pad is held. Note that parse() still had to COUNT its two data
  // bytes - a status it skipped by status alone would leave them to be read as the next
  // message - which is why the flood goes through the ordinary path and dies here.
  if (type == kNoteOn || type == kNoteOff) {
    const uint8_t pad = padForNote(data1);
    if (pad >= kNumPads) return;
    const bool pressed = type == kNoteOn && data2 > 0;
    // The pads are velocity sensitive, and that is what a recorded note is worth.
    push(kGroupPad, pad, pressed, pressed ? data2 : 0);
    return;
  }
  if (type != kControlChange) return;

  // The round buttons are CCs, not notes - the one place this device differs in kind from the
  // APC, whose every control is a note.
  const bool pressed = data2 > 0;
  uint8_t button = 0;
  if (rightButtonForCc(data1, button)) {
    push(kGroupRight, button, pressed, 0);
  } else if (bottomButtonForCc(data1, button)) {
    push(kGroupBottom, button, pressed, 0);
  }
}

void LaunchpadXSurface::push(uint8_t group, uint8_t index, bool pressed, uint8_t velocity) {
  if (queueCount_ == kQueueSize) return;  // far behind: drop rather than block
  ControlEvent& e = queue_[(queueHead_ + queueCount_) % kQueueSize];
  e.group = group;
  e.index = index;
  e.pressed = pressed;
  e.value = 0;
  e.initial = false;
  e.velocity = velocity;
  ++queueCount_;
}

// One SysEx a frame carrying every LED that changed, pads and buttons together: the device
// numbers them all in one coordinate system, so they need no separate messages. The buttons
// go whenever they change - there are sixteen and a hand is waiting on them - and the pads
// take turns, because messages sent in a burst get lost.
void LaunchpadXSurface::show(const LedFrame& frame) {
  uint8_t message[sizeof(kHeader) + 2 + 5 * (kLedsPerShow + kNumRightButtons + kNumBottomButtons)];
  size_t at = sizeof(kHeader);
  memcpy(message, kHeader, sizeof(kHeader));
  message[at++] = kLedMessage;
  const size_t firstSpec = at;

  for (uint8_t i = 0; i < kNumRightButtons; ++i) {
    if (ledsKnown_ && same(rightShown_[i], frame.right[i])) continue;
    message[at++] = kRgbSpec;
    message[at++] = ccForRightButton(i);
    message[at++] = channel7(frame.right[i].r);
    message[at++] = channel7(frame.right[i].g);
    message[at++] = channel7(frame.right[i].b);
    rightShown_[i] = frame.right[i];
  }
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) {
    if (ledsKnown_ && same(bottomShown_[i], frame.bottom[i])) continue;
    message[at++] = kRgbSpec;
    message[at++] = ccForBottomButton(i);
    message[at++] = channel7(frame.bottom[i].r);
    message[at++] = channel7(frame.bottom[i].g);
    message[at++] = channel7(frame.bottom[i].b);
    bottomShown_[i] = frame.bottom[i];
  }

  uint8_t sent = 0;
  uint8_t lastPad = 0;
  for (uint8_t step = 0; step < kNumPads && sent < kLedsPerShow; ++step) {
    const uint8_t pad = static_cast<uint8_t>((nextPad_ + step) % kNumPads);
    if (ledsKnown_ && same(padShown_[pad], frame.pads[pad])) continue;
    message[at++] = kRgbSpec;
    message[at++] = noteForPad(pad);
    message[at++] = channel7(frame.pads[pad].r);
    message[at++] = channel7(frame.pads[pad].g);
    message[at++] = channel7(frame.pads[pad].b);
    padShown_[pad] = frame.pads[pad];
    lastPad = pad;
    ++sent;
  }
  if (sent > 0) nextPad_ = static_cast<uint8_t>((lastPad + 1) % kNumPads);

  ledsKnown_ = true;
  if (at == firstSpec) return;  // nothing changed; the header alone is not worth sending
  message[at++] = kSysExEnd;
  port_.write(message, at);
}

}  // namespace gx
