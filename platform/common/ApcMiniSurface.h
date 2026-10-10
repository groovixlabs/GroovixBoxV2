#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/MidiPort.h"
#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface for the Akai APC mini mk2, which has exactly this layout: 8x8 RGB pads, 8
// buttons right of the grid (our R1..R8, green LEDs), 8 below it (B1..B8, red LEDs), Shift,
// 8 faders and a master fader. It speaks the mk2 Communications Protocol v1.0 over the
// device's first MIDI port ("Control"):
//  - pads send notes 0..63 from the bottom-left, row by row upwards; R1..R8 are notes
//    112..119 from the top, B1..B8 notes 100..107, Shift note 122, the faders CC 48..56
//  - a pad's colour is a Note On: the channel picks the brightness (0x90 is 10% and 0x96
//    100%) and the velocity picks one of the device's 128 fixed colours. Three bytes a pad,
//    against eight inside an RGB SysEx, so a frame carries more of the grid. The
//    single-colour buttons are on for a bright colour and off for the dim ones the UI uses
//    for idle states
//  - begin() sends the Introduction message, and the device answers with every fader's
//    position
//  - Shift + R6 and R7 are the device's own Drum and Note pad modes. It handles them itself
//    and never passes them on, so nothing of ours may live there; the same buttons bring it
//    back, the way any mode button does
// LED updates are paced, because messages sent in a burst get lost: each frame sends at most
// 8 changed pads in one short SysEx, taking turns round the grid, and nothing is sent until
// the device has answered the Introduction message (or about half a second has passed).
// No heap or OS calls, so it runs wherever a MidiPort can.
class ApcMiniSurface : public ControlSurface {
 public:
  explicit ApcMiniSurface(MidiPort& port);
  ~ApcMiniSurface();  // turns the LEDs off

  // Wakes the device and asks for the fader positions. Call once before polling.
  bool begin();
  bool pollEvent(ControlEvent& event) override;
  // What the device shows for a pad message: the palette colour at the channel's brightness.
  // The surface picks the pair whose result is nearest the colour the UI asked for.
  static Rgb ledColor(uint8_t channel, uint8_t velocity);
  // The other direction: the message for a colour, packed as (channel << 8) | velocity, with
  // 0 for black. Public because the APC in its other role - ApcMiniPanelSurface - is the same
  // hardware and must light a colour identically, and because a device's colour scheme is one
  // thing however many ways the device is read.
  static uint16_t padCodeFor(Rgb color);
  void show(const LedFrame& frame) override;
  // Turns every LED off at once.
  void clearLeds();

 private:
  enum { kQueueSize = 64, kMaxSysEx = 64 };

  void parse(uint8_t byte);
  void handleMessage(uint8_t status, uint8_t data1, uint8_t data2);
  void handleSysEx();
  void push(uint8_t group, uint8_t index, bool pressed, uint16_t value, bool initial = false,
            uint8_t velocity = 0);
  void sendPads(const LedFrame& frame);
  uint16_t codeFor(Rgb color);
  void sendButtons(const LedFrame& frame);

  MidiPort& port_;
  ControlEvent queue_[kQueueSize];  // events parsed but not yet polled
  uint8_t queueHead_;
  uint8_t queueCount_;
  uint8_t runningStatus_;  // status of the message being received, 0 for none
  uint8_t data_[2];
  uint8_t dataCount_;
  bool inSysEx_;
  uint8_t sysEx_[kMaxSysEx];  // bytes between F0 and F7
  uint8_t sysExLength_;

  LedFrame sent_;        // colours last sent to each LED

  // The last few colours encoded, so a grid of repeated shades doesn't search the palette
  // again for every pad: the UI uses a couple of dozen colours in all.
  enum { kCacheSize = 16 };
  uint32_t cacheKey_[kCacheSize];
  uint16_t cacheCode_[kCacheSize];
  uint8_t cacheNext_;
  uint8_t cacheUsed_;

  uint16_t padCode_[kNumPads];  // channel and velocity each pad is showing
  uint64_t padsKnown_;   // bit n: padCode_[n] is what the pad shows
  bool buttonsKnown_;    // the buttons show sent_.right and sent_.bottom
  uint8_t nextPad_;      // where the next frame starts looking for changed pads
  bool ready_;           // the device has answered the Introduction message
  uint8_t framesWaited_;
};

}  // namespace gx
