#pragma once

#include <stddef.h>
#include <stdint.h>

#include "common/MidiPort.h"
#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface for the Akai MIDI Mix, which is the mixer panel of this instrument in
// hardware: 8 strips of three knobs, a MUTE button, a REC ARM button and a fader, with SOLO,
// BANK LEFT and BANK RIGHT beside them and a master fader. It speaks plain MIDI on its only
// port, the factory map:
//  - strip n: knobs CC base, base+1, base+2 and fader CC base+3, where base is 16, 20, 24, 28
//    for strips 1-4 and 46, 50, 54, 58 for strips 5-8; the master fader is CC 62
//  - each strip owns three notes from n = 0: MUTE 1 + 3n, SOLO 2 + 3n and REC ARM 3 + 3n.
//    While the panel's SOLO button (note 27) is held, its mute buttons send their solo notes
//    instead, so both arrive as the A1 row. BANK LEFT and RIGHT are 25 and 26
//  - a button's LED lights with a Note On to the same note, velocity 127, and goes out with
//    velocity 0. Note Off does nothing at all, and SOLO and SEND ALL have no LED to drive
//
// The MUTE row becomes the mixer's A1 row (mute, or solo while SOLO is held, which is how the
// panel is meant to be played) and REC ARM the A2 row. SOLO is the mixer's Shift. BANK LEFT
// and RIGHT move the track page, so the eight strips follow tracks 1-8, 9-16 and so on,
// taking the pads with them.
//
// While SOLO is held the panel lights every MUTE LED itself, whatever the host has set, and
// restores them on release — its own way of saying the MUTE buttons are solo buttons.
//
// It has no way to report where its knobs and faders are, so nothing is known until one is
// moved — unlike the APC, there is no start-up scan. No heap or OS calls, so it runs wherever
// a MidiPort can.
class MidiMixSurface : public ControlSurface {
 public:
  explicit MidiMixSurface(MidiPort& port);
  ~MidiMixSurface();  // turns the LEDs off

  bool pollEvent(ControlEvent& event) override;
  void show(const LedFrame& frame) override;
  void clearLeds();

  // Buttons the surface handles itself rather than passing on: BANK LEFT and RIGHT ask for
  // the track page before and after this one. The app reads and clears them each frame.
  bool takeBankStep(int8_t& step);

 private:
  enum { kQueueSize = 32 };

  void parse(uint8_t byte);
  void handleMessage(uint8_t status, uint8_t data1, uint8_t data2);
  void push(uint8_t group, uint8_t index, bool pressed, uint16_t value);
  void sendLed(uint8_t note, bool on);

  MidiPort& port_;
  ControlEvent queue_[kQueueSize];
  uint8_t queueHead_;
  uint8_t queueCount_;
  uint8_t runningStatus_;
  uint8_t data_[2];
  uint8_t dataCount_;
  bool inSysEx_;

  uint8_t ledState_[kNumMixButtons + kNumMixSideButtons];  // 1 while the LED is lit
  bool ledsKnown_;
  int8_t bankStep_;  // -1 or 1 while a bank button is waiting to be read
};

}  // namespace gx
