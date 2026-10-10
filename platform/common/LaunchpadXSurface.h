#pragma once

#include <stdint.h>

#include "common/MidiPort.h"
#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface for a Novation Launchpad X playing the GRID - the pads you sequence on.
// When one is plugged in it takes the grid and the APC mini moves to the page panel, which is
// the arrangement the rig is built around: see MidiRig and ApcMiniPanelSurface.
//
// It has 8x8 velocity-sensitive RGB pads, 8 round buttons above the grid and 8 to the right,
// and nothing else - no faders, and NO SHIFT. Sixteen edge buttons is exactly R1..R8 plus
// B1..B8 with none left over, which is why the modifier lives on the panel.
//
//   pads              the 8x8 grid
//   right column      R1..R8, top to bottom - the function buttons
//   top row           B1..B8, left to right. The Launchpad has no row BELOW the grid, so the
//                     buttons core calls "bottom" are physically above it here. They do the
//                     same job - the tracks, and the pages under Shift - and they are the only
//                     eight left
//
// ---- verified against the device, 2026-10-10 ----
//
// Measured on a Launchpad X (card 4) with aseqdump, in Programmer mode, every message below
// seen on the wire rather than taken from the documentation.
//
// THE PORT. The device offers two: "LPX DAW" and "LPX MIDI". **Everything arrives on the MIDI
// port** - not a single pad or button reached the DAW port, which only ever echoed a mode
// change back. Both ports accept the mode-change SysEx and acknowledge it, so the MIDI port
// alone does input and output and that is the one to open.
//
// ONE COORDINATE SYSTEM for the whole surface: value = row * 10 + column, row 1 at the BOTTOM
// and column 1 at the LEFT. The grid is rows 1-8 and columns 1-8, so its top-left pad is 81
// and its bottom-right is 18. Column 9 is the round buttons down the right, row 9 the round
// buttons across the top. Pads speak in notes, the round buttons in CCs, and the numbers are
// the same either way:
//
//   pads              notes 11..88   Note On pressed with a REAL VELOCITY (79, 102, 49 ...
//                                    were measured), Note Off released. Top row 81..88
//   right column      CC 89, 79, 69, 59, 49, 39, 29, 19   top to bottom: our R1..R8
//   top row           CC 91..98                           left to right: our B1..B8
//                     Both send value 127 pressed and 0 released - CC, not notes
//
// AFTERTOUCH IS A FIREHOSE AND IS IGNORED. Every pad press is followed by polyphonic
// aftertouch (0xA0), four or five messages for a tap and a steady stream while a pad is held
// - eighteen in a two-second hold, all of them for one pad. Nothing in the sequencer wants
// them, so the parser drops 0xA0 outright. It still has to COUNT their bytes, which is why
// they go through the ordinary two-data-byte path rather than being skipped by status alone.
//
// PROGRAMMER MODE. begin() sends F0 00 20 29 02 0C 0E 01 F7, which the device acknowledges by
// echoing it back. Without it the firmware keeps the grid for its own Session, Note and Custom
// modes and the layout above does not hold - the same class of trap as the APC mini keeping
// Shift + R6 and R7 for its Drum and Note modes. Going away puts it back in Live mode, so the
// next piece of software to open it finds it as it expects.
//
// LEDs are set with the RGB SysEx - F0 00 20 29 02 0C 03 {03 <index> <r> <g> <b>} ... F7,
// each channel 0..127 - which takes the UI's colours exactly, with no palette to search and
// no nearest-hue compromise of the kind the APC needs. One message carries the whole frame's
// changes, pads and buttons together, because the index is that one coordinate system.
//
// No heap or OS calls, so it runs wherever a MidiPort can.
class LaunchpadXSurface : public ControlSurface {
 public:
  explicit LaunchpadXSurface(MidiPort& port);
  ~LaunchpadXSurface();  // darkens the LEDs and hands the device back in Live mode

  // Puts the device in Programmer mode and darkens it. Call once before polling.
  bool begin();
  bool pollEvent(ControlEvent& event) override;
  void show(const LedFrame& frame) override;
  void clearLeds();

  // The device's number for one of our pads, and for a function or track button: public
  // because it is the whole map, and a test that cannot read it can only repeat it.
  static uint8_t noteForPad(uint8_t pad);
  static uint8_t ccForRightButton(uint8_t button);
  static uint8_t ccForBottomButton(uint8_t button);

 private:
  enum { kQueueSize = 64, kLedsPerShow = 24 };

  void parse(uint8_t byte);
  void handleMessage(uint8_t status, uint8_t data1, uint8_t data2);
  void push(uint8_t group, uint8_t index, bool pressed, uint8_t velocity);
  void setMode(bool programmer);

  MidiPort& port_;
  ControlEvent queue_[kQueueSize];
  uint8_t queueHead_;
  uint8_t queueCount_;
  uint8_t runningStatus_;
  uint8_t data_[2];
  uint8_t dataCount_;
  bool inSysEx_;

  Rgb padShown_[kNumPads];
  Rgb rightShown_[kNumRightButtons];
  Rgb bottomShown_[kNumBottomButtons];
  bool ledsKnown_;   // the arrays above say what the device is showing
  uint8_t nextPad_;  // where the next frame starts looking for changed pads
};

}  // namespace gx
