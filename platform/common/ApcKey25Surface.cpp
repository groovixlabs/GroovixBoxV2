#include "common/ApcKey25Surface.h"

#include <string.h>

#include "ui/Palette.h"

namespace gx {

namespace {

const uint8_t kNoteOff = 0x80;
const uint8_t kNoteOn = 0x90;
const uint8_t kSysExStart = 0xF0;
const uint8_t kFirstRealTime = 0xF8;

// The pads are notes 0..39 from the bottom-left, ascending left to right and upwards, so the
// top row is 32..39. The panel's rows run the other way - row 0 is the top one, the first kind
// of page - so the row is flipped and the column is not.
//
// The device has five rows and the panel wants five: the first three kinds of page from the
// top, then the preset windows, then the preset pages on the bottom row. Controls.h settles
// which row each job sits on from the row count, so this panel and the APC mini's lay out the
// same way - see panelRowJobAt.
const uint8_t kPadRows = 5;  // the device's own, which here is exactly the panel's
const uint8_t kNumPadNotes = kPadRows * kPagesPerKind;
static_assert(kNumPanelRows <= kPadRows, "a pad row for the pages and one for the windows");

uint8_t noteForRow(uint8_t rowFromTop, uint8_t column) {
  return static_cast<uint8_t>((kPadRows - 1 - rowFromTop) * kPagesPerKind + column);
}

uint8_t noteForPagePad(uint8_t pad) {
  const uint8_t pageRow = static_cast<uint8_t>(pad / kPagesPerKind);
  return noteForRow(panelPageRowAt(kPadRows, pageRow),
                    static_cast<uint8_t>(pad % kPagesPerKind));
}

uint8_t noteForWindowPad(uint8_t window) {
  return noteForRow(panelWindowRowAt(kPadRows), window);
}

bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// The eight buttons under the grid: KB1..KB4 are the arrows, and KB5..KB8 are spare. The
// preset windows had them while there were only four of them; a pad row holds all eight.
const uint8_t kFirstArrowNote = 64;   // KB1..KB4: up, down, left, right

// The mode buttons are the right-hand column, top to bottom. Five of its six buttons are
// used: KR6 is note 81, below the run and the only one with no LED, so it stays out of a
// group whose whole job is to show which mode is open.
const uint8_t kFirstModeNote = 82;  // KR1..KR5 are notes 82..86

// Pads sent in one frame. The mk2 loses LED messages sent in a burst and this device is no
// likelier to survive one, so a frame sends a few and the next frame carries on round the
// grid: a page switch moves two pads, and only the first draw has all forty to get through.
const uint8_t kPadsPerFrame = 8;

}  // namespace

// The page panel's colours are a closed set - see UiController::pageRowPadColor - so each is
// named here rather than searched for. Matching by nearest hue would be wrong as well as
// unnecessary: "holds data" is dim blue and "empty" is grey, which a green/red/yellow pad
// cannot tell apart by distance but must tell apart to be worth looking at.
uint8_t ApcKey25Surface::padColorFor(Rgb color) {
  if (!isLit(color)) return kPadOff;                 // no such page
  // The step page the playhead is on. The UI blinks it by alternating this colour with the
  // page's own, so the pad blinks whatever a pad is capable of - the device's own blink would
  // only add a second rate on top of that one, so it is not used here.
  if (same(color, kWhite)) return kPadGreen;
  if (same(color, kSelectedColor)) return kPadGreen;  // the page that is open
  if (same(color, kFilledColor)) return kPadYellow;   // a page with something on it
  return kPadRed;                                     // a page with nothing on it
}

// The buttons are one colour with on, off and blink - not the pads' seven values, where 4 and
// 6 blink and 3 and 5 are other colours. Three states is exactly what a window needs, and the
// one you are on is the one that should not be competing for attention, so it is the steady
// one: solid where you are, blinking where you could go, dark where there is nothing.
uint8_t ApcKey25Surface::buttonColorFor(Rgb color) {
  if (!isLit(color)) return kPadOff;
  if (same(color, kSelectedColor)) return kButtonOn;
  if (same(color, kFilledColor)) return kButtonBlink;
  return kPadOff;  // past the end of the device's list: nothing to go to
}

ApcKey25Surface::ApcKey25Surface(MidiPort& port)
    : port_(port),
      queueHead_(0),
      queueCount_(0),
      runningStatus_(0),
      dataCount_(0),
      nextPad_(0),
      padsKnown_(false) {
  data_[0] = 0;
  data_[1] = 0;
  memset(padShown_, 0, sizeof(padShown_));
  memset(modeShown_, 0, sizeof(modeShown_));
  memset(arrowShown_, 0, sizeof(arrowShown_));
  clearLeds();
}

ApcKey25Surface::~ApcKey25Surface() { clearLeds(); }

void ApcKey25Surface::clearLeds() {
  // Every button under the grid, not only the four the arrows use: KB5..KB8 are spare now and
  // this is the one place that darkens them.
  uint8_t message[3 * (kNumPadNotes + kNumBottomButtons + kNumPanelModes)];
  size_t at = 0;
  for (uint8_t note = 0; note < kNumPadNotes; ++note) {
    message[at++] = kNoteOn;
    message[at++] = note;
    message[at++] = kPadOff;
  }
  for (uint8_t button = 0; button < kNumBottomButtons; ++button) {
    message[at++] = kNoteOn;
    message[at++] = static_cast<uint8_t>(kFirstArrowNote + button);
    message[at++] = kButtonOff;
  }
  for (uint8_t button = 0; button < kNumPanelModes; ++button) {
    message[at++] = kNoteOn;
    message[at++] = static_cast<uint8_t>(kFirstModeNote + button);
    message[at++] = kButtonOff;
  }
  port_.write(message, at);
  memset(padShown_, kPadOff, sizeof(padShown_));
  memset(modeShown_, kButtonOff, sizeof(modeShown_));
  memset(arrowShown_, kButtonOff, sizeof(arrowShown_));
  padsKnown_ = true;
}

bool ApcKey25Surface::pollEvent(ControlEvent& event) {
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

void ApcKey25Surface::parse(uint8_t byte) {
  if (byte >= kFirstRealTime) return;
  if (byte & 0x80) {
    runningStatus_ = byte < kSysExStart ? byte : 0;
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

void ApcKey25Surface::handleMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  if ((status & 0x0F) != 0) return;  // the pads speak on channel 1
  const uint8_t type = status & 0xF0;
  if (type != kNoteOn && type != kNoteOff) return;
  // The pads and the four arrow buttons; the knobs and the other ten buttons have no job
  // here. The 25 keys are on this port too but on channel 2, so the test above has already
  // dropped them - which is the only thing that reliably does: shifted two octaves down the
  // lowest key is note 12, well inside the pads' own notes.
  // Nothing on this device is velocity sensitive except the keys, and KR6 releases at 0 where
  // the rest release at 127 - so a press is the status byte, never the velocity.
  const bool pressed = type == kNoteOn && data2 > 0;
  if (data1 < kNumPadNotes) {
    const uint8_t rowFromTop = static_cast<uint8_t>(kPadRows - 1 - data1 / kPagesPerKind);
    const uint8_t column = static_cast<uint8_t>(data1 % kPagesPerKind);
    uint8_t pageRow = 0;
    switch (panelRowJobAt(kPadRows, rowFromTop, pageRow)) {
      case kPanelRowPages:
        push(kGroupPage, pagePadIndex(pageRow, column), pressed);
        break;
      case kPanelRowWindows:
        push(kGroupPresetWindow, column, pressed);
        break;
      default:
        break;  // this device has no spare row, but the rule is the panel's, not its own
    }
  } else if (data1 >= kFirstArrowNote && data1 < kFirstArrowNote + kNumPanelArrows) {
    push(kGroupPanelArrow, static_cast<uint8_t>(data1 - kFirstArrowNote), pressed);
  } else if (data1 >= kFirstModeNote && data1 < kFirstModeNote + kNumPanelModes) {
    push(kGroupPanelMode, static_cast<uint8_t>(data1 - kFirstModeNote), pressed);
  }
}

void ApcKey25Surface::push(uint8_t group, uint8_t index, bool pressed) {
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

void ApcKey25Surface::show(const LedFrame& frame) {
  uint8_t message[3 * (kPadsPerFrame + kNumPanelArrows + kNumPanelModes)];
  size_t at = 0;
  uint8_t sent = 0;
  // The nine buttons go every frame they change: there are few of them, so they never need
  // the pacing the forty pads do, and they are what a hand is waiting on.
  for (uint8_t arrow = 0; arrow < kNumPanelArrows; ++arrow) {
    const uint8_t want = buttonColorFor(frame.panelArrows[arrow]);
    if (padsKnown_ && arrowShown_[arrow] == want) continue;
    message[at++] = kNoteOn;
    message[at++] = static_cast<uint8_t>(kFirstArrowNote + arrow);
    message[at++] = want;
    arrowShown_[arrow] = want;
  }
  for (uint8_t button = 0; button < kNumPanelModes; ++button) {
    const uint8_t want = buttonColorFor(frame.panelModes[button]);
    if (padsKnown_ && modeShown_[button] == want) continue;
    message[at++] = kNoteOn;
    message[at++] = static_cast<uint8_t>(kFirstModeNote + button);
    message[at++] = want;
    modeShown_[button] = want;
  }
  // Round the grid from where the last frame stopped, so a pad that changes while others are
  // still going out is not held back behind them for good. The window row rounds with the
  // page rows: it is a row of pads like any other, and one counter keeps the pacing in one
  // place.
  for (uint8_t step = 0; step < kNumPanelPadLeds && sent < kPadsPerFrame; ++step) {
    const uint8_t led = static_cast<uint8_t>((nextPad_ + step) % kNumPanelPadLeds);
    const bool window = led >= kNumPagePads;
    const uint8_t index = window ? static_cast<uint8_t>(led - kNumPagePads) : led;
    const uint8_t want =
        padColorFor(window ? frame.pageWindows[index] : frame.pages[index]);
    if (padsKnown_ && padShown_[led] == want) continue;
    message[at++] = kNoteOn;
    message[at++] = window ? noteForWindowPad(index) : noteForPagePad(index);
    message[at++] = want;
    padShown_[led] = want;
    ++sent;
    nextPad_ = static_cast<uint8_t>((led + 1) % kNumPanelPadLeds);
  }
  if (at != 0) port_.write(message, at);
  padsKnown_ = true;
}

}  // namespace gx
