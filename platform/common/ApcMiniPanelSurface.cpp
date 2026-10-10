#include "common/ApcMiniPanelSurface.h"

#include <string.h>

#include "common/ApcMiniSurface.h"

namespace gx {

namespace {

// Values from the APC mini mk2 Communications Protocol v1.0, as in ApcMiniSurface.
const uint8_t kNumPadNotes = 64;       // notes 0..63, 0 = bottom-left pad
const uint8_t kFirstTrackNote = 0x64;  // the 8 buttons under the grid: B1..B8
const uint8_t kFirstSceneNote = 0x70;  // the 8 right of the grid, top down: R1..R8
const uint8_t kShiftNote = 0x7A;

const uint8_t kNoteOff = 0x80;
const uint8_t kNoteOn = 0x90;
const uint8_t kSysExStart = 0xF0;
const uint8_t kSysExEnd = 0xF7;
const uint8_t kFirstRealTime = 0xF8;

const uint8_t kAkaiId = 0x47;
const uint8_t kDeviceId = 0x7F;
const uint8_t kModelId = 0x4F;
const uint8_t kIntroductionMessage = 0x60;

// Button LEDs are one colour: on for a bright UI colour, off for the dim ones.
const uint8_t kButtonOnLevel = 200;
const uint8_t kButtonLedOff = 0x00;
const uint8_t kButtonLedOn = 0x01;

// At most this many pads change per frame; bursts get lost. The panel has well under half the
// pads in play that the grid has, and most frames change one or two.
const uint8_t kPadsPerShow = 16;

// The panel's pad LEDs in one round: the page rows, then the window row after them.
const uint8_t kNumPanelPadLeds = kNumPagePads + kNumPanelWindows;

// The buttons under and beside the grid split the way the Key 25's do, so the two panels are
// the same instrument: B1..B4 the arrows, R1..R5 the modes. B5..B8 are spare - the windows had
// them while there were only four, and a row of pads holds all eight.
const uint8_t kFirstArrowButton = 0;

// Our row (0 at the top) and column as the device's note (row 0 at the bottom).
uint8_t noteForRow(uint8_t rowFromTop, uint8_t column) {
  return static_cast<uint8_t>((kGridRows - 1 - rowFromTop) * kGridCols + column);
}

// The device's note for a page pad and for a window pad: where Controls.h puts those rows on
// a device with this many pad rows.
uint8_t noteForPagePad(uint8_t pad) {
  const uint8_t pageRow = static_cast<uint8_t>(pad / kPagesPerKind);
  return noteForRow(panelPageRowAt(kGridRows, pageRow), static_cast<uint8_t>(pad % kPagesPerKind));
}

uint8_t noteForWindowPad(uint8_t window) {
  return noteForRow(panelWindowRowAt(kGridRows), window);
}

bool buttonLit(Rgb c) {
  return c.r >= kButtonOnLevel || c.g >= kButtonOnLevel || c.b >= kButtonOnLevel;
}

bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

}  // namespace

static_assert(kPagesPerKind == kGridCols, "a page of a kind per pad across a row");
static_assert(kNumPanelRows <= kGridRows, "a pad row for the pages and one for the windows");
static_assert(kNumPanelModes <= kNumRightButtons, "a right-hand button per mode button");
static_assert(kNumPanelArrows <= kNumBottomButtons, "an arrow per button under the grid");

ApcMiniPanelSurface::ApcMiniPanelSurface(MidiPort& port)
    : port_(port),
      queueHead_(0),
      queueCount_(0),
      runningStatus_(0),
      dataCount_(0),
      inSysEx_(false),
      ledsKnown_(false),
      nextPad_(0) {
  memset(pageShown_, 0, sizeof(pageShown_));
  memset(modeShown_, 0, sizeof(modeShown_));
  memset(arrowShown_, 0, sizeof(arrowShown_));
  memset(windowShown_, 0, sizeof(windowShown_));
}

ApcMiniPanelSurface::~ApcMiniPanelSurface() { clearLeds(); }

bool ApcMiniPanelSurface::begin() {
  // Application 0, version 1.0.0 - the same Introduction the grid role sends. The reply
  // carries the fader positions, which this role has no use for, so nothing waits for it.
  const uint8_t intro[] = {kSysExStart, kAkaiId, kDeviceId, kModelId, kIntroductionMessage,
                           0x00,        0x04,    0x00,      0x01,     0x00,
                           0x00,        kSysExEnd};
  return port_.write(intro, sizeof(intro));
}

bool ApcMiniPanelSurface::pollEvent(ControlEvent& event) {
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

void ApcMiniPanelSurface::show(const LedFrame& frame) {
  sendButtons(frame);
  sendPads(frame);
  ledsKnown_ = true;
}

void ApcMiniPanelSurface::clearLeds() {
  // Velocity 0 is off whatever the channel, so the whole grid is 64 short messages. Every
  // pad, not only the ones the pages use: the spare rows must go dark too.
  uint8_t message[(kNumPadNotes + kNumRightButtons + kNumBottomButtons) * 3];
  size_t size = 0;
  for (uint8_t note = 0; note < kNumPadNotes; ++note) {
    message[size++] = kNoteOn;
    message[size++] = note;
    message[size++] = 0;
  }
  for (uint8_t i = 0; i < kNumRightButtons; ++i) {
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstSceneNote + i);
    message[size++] = kButtonLedOff;
  }
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) {
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstTrackNote + i);
    message[size++] = kButtonLedOff;
  }
  port_.write(message, size);
  memset(pageShown_, 0, sizeof(pageShown_));
  memset(modeShown_, 0, sizeof(modeShown_));
  memset(arrowShown_, 0, sizeof(arrowShown_));
  memset(windowShown_, 0, sizeof(windowShown_));
  ledsKnown_ = true;
}

void ApcMiniPanelSurface::parse(uint8_t byte) {
  if (byte >= kFirstRealTime) return;
  if (byte == kSysExStart) {
    inSysEx_ = true;
    return;
  }
  if (inSysEx_) {
    // Nothing in this role reads SysEx - the Introduction reply is only fader positions - so
    // it is swallowed whole rather than collected.
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

void ApcMiniPanelSurface::handleMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  // The device's own Drum and Note modes move the pads to other channels; only its default
  // mode, on channel 1, drives the sequencer.
  if ((status & 0x0F) != 0) return;
  const uint8_t type = status & 0xF0;
  // Notes only, which is also what drops the nine faders: they have no job in this role,
  // since the mixer is the MIDI Mix and a fader that moved something from two devices would
  // be two mechanisms for one thing.
  if (type != kNoteOn && type != kNoteOff) return;
  const bool pressed = type == kNoteOn && data2 > 0;

  if (data1 < kNumPadNotes) {
    const uint8_t rowFromTop = static_cast<uint8_t>(kGridRows - 1 - data1 / kGridCols);
    const uint8_t column = static_cast<uint8_t>(data1 % kGridCols);
    uint8_t pageRow = 0;
    // The spare rows report nothing rather than an index past the panel.
    switch (panelRowJobAt(kGridRows, rowFromTop, pageRow)) {
      case kPanelRowPages:
        push(kGroupPage, pagePadIndex(pageRow, column), pressed);
        break;
      case kPanelRowWindows:
        push(kGroupPresetWindow, column, pressed);
        break;
      default:
        break;
    }
  } else if (data1 >= kFirstSceneNote && data1 < kFirstSceneNote + kNumRightButtons) {
    const uint8_t button = static_cast<uint8_t>(data1 - kFirstSceneNote);
    if (button < kNumPanelModes) push(kGroupPanelMode, button, pressed);
  } else if (data1 >= kFirstTrackNote && data1 < kFirstTrackNote + kNumBottomButtons) {
    // B1..B4 are the arrows; B5..B8 are spare and say nothing.
    const uint8_t button = static_cast<uint8_t>(data1 - kFirstTrackNote);
    if (button < kFirstArrowButton + kNumPanelArrows) {
      push(kGroupPanelArrow, static_cast<uint8_t>(button - kFirstArrowButton), pressed);
    }
  } else if (data1 == kShiftNote) {
    // The whole reason the APC is the panel: Shift is a role, not a device, so core takes
    // this exactly as it takes the grid's own Shift.
    push(kGroupShift, 0, pressed);
  }
}

void ApcMiniPanelSurface::push(uint8_t group, uint8_t index, bool pressed) {
  if (queueCount_ == kQueueSize) return;  // far behind: drop rather than block
  ControlEvent& e = queue_[(queueHead_ + queueCount_) % kQueueSize];
  e.group = group;
  e.index = index;
  e.pressed = pressed;
  e.value = 0;
  e.initial = false;
  e.velocity = 0;
  ++queueCount_;
}

// Round the panel's pads from where the last frame stopped, so a pad that changes while others
// are still going out is not held back behind them for good. The window row is in the round
// with the page rows: it is a row of pads like any other now, and one counter keeps the pacing
// in one place.
void ApcMiniPanelSurface::sendPads(const LedFrame& frame) {
  uint8_t message[kPadsPerShow * 3];
  size_t size = 0;
  uint8_t sent = 0;
  uint8_t lastPad = 0;
  for (uint8_t step = 0; step < kNumPanelPadLeds && sent < kPadsPerShow; ++step) {
    const uint8_t at = static_cast<uint8_t>((nextPad_ + step) % kNumPanelPadLeds);
    const bool window = at >= kNumPagePads;
    const uint8_t index = window ? static_cast<uint8_t>(at - kNumPagePads) : at;
    const Rgb want = window ? frame.pageWindows[index] : frame.pages[index];
    Rgb& shown = window ? windowShown_[index] : pageShown_[index];
    if (ledsKnown_ && same(shown, want)) continue;
    const uint16_t code = ApcMiniSurface::padCodeFor(want);
    message[size++] = static_cast<uint8_t>(kNoteOn | (code >> 8));
    message[size++] = window ? noteForWindowPad(index) : noteForPagePad(index);
    message[size++] = static_cast<uint8_t>(code & 0xFF);
    shown = want;
    lastPad = at;
    ++sent;
  }
  if (sent == 0) return;
  nextPad_ = static_cast<uint8_t>((lastPad + 1) % kNumPanelPadLeds);
  port_.write(message, size);
}

// The nine buttons go every frame they change: there are few of them, so they never need the
// pacing the pads do, and they are what a hand is waiting on. B5..B8 are not here because
// nothing lights them - clearLeds darkened them at the start and nothing writes them since.
void ApcMiniPanelSurface::sendButtons(const LedFrame& frame) {
  uint8_t message[3 * (kNumPanelModes + kNumPanelArrows)];
  size_t size = 0;
  for (uint8_t i = 0; i < kNumPanelModes; ++i) {
    const Rgb want = frame.panelModes[i];
    if (ledsKnown_ && buttonLit(modeShown_[i]) == buttonLit(want)) continue;
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstSceneNote + i);
    message[size++] = buttonLit(want) ? kButtonLedOn : kButtonLedOff;
    modeShown_[i] = want;
  }
  for (uint8_t i = 0; i < kNumPanelArrows; ++i) {
    const Rgb want = frame.panelArrows[i];
    if (ledsKnown_ && buttonLit(arrowShown_[i]) == buttonLit(want)) continue;
    message[size++] = kNoteOn;
    message[size++] = static_cast<uint8_t>(kFirstTrackNote + kFirstArrowButton + i);
    message[size++] = buttonLit(want) ? kButtonLedOn : kButtonLedOff;
    arrowShown_[i] = want;
  }
  if (size > 0) port_.write(message, size);
}

}  // namespace gx
