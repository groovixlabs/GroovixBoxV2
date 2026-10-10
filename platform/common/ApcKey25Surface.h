#pragma once

#include <stdint.h>

#include "common/MidiPort.h"
#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface for the Akai APC Key 25 mk1, used as a page panel and nothing else.
//
// The device has 5x8 pads, 8 buttons below them, 6 right of them, 8 knobs and 25 keys. Only
// the pads and four of the buttons are taken, and only for pages: a row for each kind of page
// and a pad for each page of it, lit to show which page is open and pressed to open another.
// It plays no notes and edits nothing, so the grid's layout is no concern of its - which is
// why five rows are enough here where they would be three short of the sequencer's grid.
//
// Rows, top to bottom: step pages, track pages, project pages, preset pages. There are four
// kinds of page and five rows, so the panel takes the top four and the bottom row is spare -
// dark, and it reports nothing. Patterns had a row until the grid came to hold all 64 of a
// track's patterns at once, which left nothing to page.
// KB1..KB4 are up, down, left and right: they nudge a piano roll one step or one scale degree
// at a time, the same as the APC's own pad cluster but without the Shift it needs. They are
// dark when no roll is on screen. KB5..KB8 pick preset mode's 512-voice window, the one page row whose
// eight pads are not the whole story.
//
// KR1..KR5, the top five of the right-hand column, open scale, project, scenes, the
// settings and preset - the five modes that need Shift on the APC, each one press here - and
// light to show which is open. KR6 is left out: it is the one button on the device with no LED, and a
// mode button that cannot say whether its mode is open is worse than no button.
//
// The whole device, measured on one (USB 09e8:0027, a single "Legacy" port). Everything is
// channel 1 except where it says otherwise, and every button sends Note On velocity 127
// pressed and Note Off velocity 127 released unless noted:
//
//   pads              notes 0..39   note 0 bottom-left, ascending left to right and upwards,
//                                   so the top row is 32..39 - the mk2's convention. Not
//                                   velocity sensitive: 127 every time, pressed and released
//   8 below the grid  notes 64..71  left to right
//   6 right of it     notes 82..86  top to bottom for the first five, and then
//                     note 81       the sixth and lowest - the run is not contiguous, and
//                                   this one alone releases with velocity 0
//   8 knobs           CC 48..55     absolute 0..127 over full travel, not relative. The same
//                                   CCs the mk2 uses for its faders, on another port
//   25 keys           CHANNEL 2     notes 48..72 at the default octave, velocity sensitive
//   Sustain           CHANNEL 2     CC 64, 127 pressed and 0 released
//   Octave Down/Up    nothing       the device transposes the keys itself and sends no MIDI
//                                   for either, so the octave cannot be read - and need not
//                                   be, since the notes arrive already transposed
//   Play/Pause        note 91
//   REC               note 93       (note 92 is unused on this model)
//   Shift             note 98       a plain button: the pads go on reporting while it is held
//                                   (shift down, pad down, pad up, shift up, all seen), so
//                                   shift combinations are ours to use if we ever want them
//
// So the channel is what separates keys from pads, never the note: two octaves down the
// lowest key is note 12, inside the pads' own range.
//
// A pad's colour is a Note On whose velocity picks it, and all seven were read off the device
// rather than taken from the convention: 0 off, 1 green, 2 green blink, 3 red, 4 red blink,
// 5 yellow, 6 yellow blink. Nothing like the mk2's 128 colours and brightness channels, and
// no RGB SysEx at all - three colours is the whole budget, which is why the page colours are
// matched by meaning (see padColorFor) and not by nearest hue.
//
// Some of the buttons light as well, and they do NOT follow the pads' scheme: each is one
// fixed colour with 0 off, 1 on, 2 blink, and every other value simply on (3, 4, 5, 6 and 127
// all came up steady, where 4 and 6 would blink on a pad).
//
//   notes 64..71   the 8 under the grid    RED
//   notes 82..86   KR1..KR5                GREEN
//
// And that is all of them. Every note from 72 to 127 was sent velocity 1 at once with the rest
// dark, and only 82..86 lit - so KR6 (note 81), Play/Pause (91), REC (93), Shift (98) and the
// knobs' buttons have no LEDs at all, and there is no other addressable light on the device.
// KR6 is input only: a button that reports but cannot be shown. 53 LEDs in total, 40 of them
// pads.
//
// The keys, knobs and buttons are left alone - the channel test drops the keys for free.
// No heap or OS calls, so it runs wherever a MidiPort can.
class ApcKey25Surface : public ControlSurface {
 public:
  explicit ApcKey25Surface(MidiPort& port);
  ~ApcKey25Surface();  // turns the LEDs off

  bool pollEvent(ControlEvent& event) override;
  // Lights the pads from frame.pages; every other field is another device's business.
  void show(const LedFrame& frame) override;
  void clearLeds();

  // What the device can show, as the velocity that asks for it. Each one confirmed by eye on
  // the user's unit, a column of pads per velocity.
  enum PadColor {
    kPadOff = 0,
    kPadGreen = 1,
    kPadGreenBlink = 2,
    kPadRed = 3,
    kPadRedBlink = 4,
    kPadYellow = 5,
    kPadYellowBlink = 6,
  };
  // And what the single-colour buttons under the grid can show, which is not the same list.
  enum ButtonColor {
    kButtonOff = 0,
    kButtonOn = 1,
    kButtonBlink = 2,
  };
  // Which of those a page colour becomes. The page panel uses a closed set of five colours
  // (see UiController::pageRowPadColor), so this is a match on each rather than a search for
  // the nearest - dim blue and grey are no distance apart to a pad that has neither.
  static uint8_t padColorFor(Rgb color);
  // And for a window button, whose three states are not the pads' seven.
  static uint8_t buttonColorFor(Rgb color);

 private:
  // Every pad the panel lights: the page rows, then the window row after them.
  enum { kQueueSize = 32, kNumPanelPadLeds = kNumPagePads + kNumPanelWindows };

  void parse(uint8_t byte);
  void handleMessage(uint8_t status, uint8_t data1, uint8_t data2);
  void push(uint8_t group, uint8_t index, bool pressed);

  MidiPort& port_;
  ControlEvent queue_[kQueueSize];
  uint8_t queueHead_;
  uint8_t queueCount_;
  uint8_t runningStatus_;
  uint8_t data_[2];
  uint8_t dataCount_;

  uint8_t padShown_[kNumPanelPadLeds];  // velocity each pad was last sent
  uint8_t nextPad_;                     // where the next frame starts looking for changes
  uint8_t arrowShown_[kNumPanelArrows];
  uint8_t modeShown_[kNumPanelModes];
  bool padsKnown_;                      // the three *Shown_ are what the device shows
};

}  // namespace gx
