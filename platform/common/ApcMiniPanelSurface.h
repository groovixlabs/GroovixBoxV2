#pragma once

#include <stdint.h>

#include "common/MidiPort.h"
#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface for an Akai APC mini mk2 playing the PAGE PANEL, not the grid. The same
// hardware ApcMiniSurface drives as the sequencer, read a second way: when a Launchpad X is
// plugged in it becomes the sequencer and the APC moves to this role, which is what the rig
// is for - always a grid and a panel, each on the device that suits it.
//
// A panel shows which page of each kind is open and switches to another, carries the mode
// buttons, the roll arrows and the preset windows, and - the reason the APC is wanted here
// rather than the Key 25 - it carries SHIFT, which the Launchpad X has no spare button for.
// It plays no notes and edits nothing.
//
// The layout is the APC Key 25's, pad for pad, so nothing has to be relearned when the panel
// changes device:
//
//   pad rows 1-3 (top)  the first kinds of page, one row each: steps, tracks, projects
//   pad rows 4-6        SPARE, dark, and they report nothing
//   pad row 7           the preset WINDOWS, 512 voices each - all eight of them
//   pad row 8 (bottom)  the preset PAGES, the eight pages of the window above
//   R1-R5               the mode buttons: scale, project, scenes, global settings, preset
//   R6-R8               SPARE, and dark
//   B1-B4               the roll arrows: up, down, left, right, in RollScroll's order
//   B5-B8               SPARE, and dark
//   SHIFT               the sequencer's SHIFT - no index, no device test in core, so a
//                       modifier from here is the same modifier as one from the grid
//
// Rows 7 and 8 are a pair on purpose: a window holds exactly eight preset pages, so window
// over pages reads downward like a row of tabs over what the open tab shows. Controls.h
// settles which pad row each job sits on, from the count of rows the device has, so the Key
// 25's five rows lay out the same way - the gap in the middle is the only difference.
//
// The nine faders have no job in this role: the mixer is the MIDI Mix, and a fader that moved
// something from two devices would be two mechanisms for one thing. They are read and dropped.
//
// Colours come from ApcMiniSurface's palette, which is the device's own, so a page reads the
// same whichever role the APC is playing. LED updates are paced the same way too: messages
// sent in a burst get lost.
//
// Verified MIDI map - the same device as ApcMiniSurface, so the same map:
//   pads              notes 0..63   note 0 is the BOTTOM-left pad, ascending left to right
//                                   and upwards, so the top row is 56..63
//   8 right of grid   notes 112..119  R1..R8, top down
//   8 below the grid  notes 100..107  B1..B8, left to right
//   Shift             note 122
//   faders            CC 48..56     ignored here
// Pads are RGB (channel = brightness, velocity = palette entry); R and B buttons are single
// colour, on or off. Shift has no LED.
//
// No heap or OS calls, so it runs wherever a MidiPort can.
class ApcMiniPanelSurface : public ControlSurface {
 public:
  explicit ApcMiniPanelSurface(MidiPort& port);
  ~ApcMiniPanelSurface();  // turns the LEDs off

  // Wakes the device. Call once before polling. Unlike the grid role nothing is asked for in
  // return - the faders have no job here, so their positions are of no interest.
  bool begin();
  bool pollEvent(ControlEvent& event) override;
  void show(const LedFrame& frame) override;
  void clearLeds();

 private:
  enum { kQueueSize = 32, kMaxSysEx = 64 };

  void parse(uint8_t byte);
  void handleMessage(uint8_t status, uint8_t data1, uint8_t data2);
  void push(uint8_t group, uint8_t index, bool pressed);
  void sendPads(const LedFrame& frame);
  void sendButtons(const LedFrame& frame);

  MidiPort& port_;
  ControlEvent queue_[kQueueSize];
  uint8_t queueHead_;
  uint8_t queueCount_;
  uint8_t runningStatus_;
  uint8_t data_[2];
  uint8_t dataCount_;
  bool inSysEx_;

  Rgb pageShown_[kNumPagePads];   // colour each page pad is showing
  Rgb modeShown_[kNumPanelModes];
  Rgb arrowShown_[kNumPanelArrows];
  Rgb windowShown_[kNumPanelWindows];
  bool ledsKnown_;    // the arrays above say what the device shows
  uint8_t nextPad_;   // where the next frame starts looking for changed page pads
};

}  // namespace gx
