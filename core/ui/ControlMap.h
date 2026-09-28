#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ui/Controls.h"

namespace gx {

static const uint8_t kNoCc = 0xFF;  // the control sends nothing
static const uint8_t kMaxCcNumber = 127;
static const uint8_t kCcCentre = 64;

// How a control's travel maps onto a CC's 0..127.
enum CcSweep {
  kSweepFull = 0,  // bottom (or fully left) to top: 0 to 127
  kSweepCentre,    // centred, like pan: the middle sends 64 and holds it in a small detent
};

struct CcAssignment {
  uint8_t cc;     // kNoCc for a control that sends nothing
  uint8_t sweep;  // CcSweep
};

// Which MIDI CC each fader and knob sends. Every control belongs to a track — the strip's
// track on the current track page — so the CC goes out on that track's channel and port; the
// two master faders follow the selected track.
//
// Read from a config file of lines like:
//   fader3    = 11           # CC 11, full sweep
//   knob3.1   = 10, center   # knob row 3 of strip 1: CC 10, centred
//   knobrow2  = 71           # all eight knobs of row 2
//   master    = off          # sends nothing
// Names: fader1..fader8, faderrow, master, mixfader1..mixfader8, mixfaderrow, mixmaster,
// knob<row>.<strip> (rows 1..3, strips 1..8) and knobrow1..knobrow3. A row name sets all
// eight strips at once. Modes are "full" (the default) and "center"/"centre". '#' or ';'
// starts a comment, and blank lines are ignored.
// The same file carries a few settings that belong to the platform rather than to a control:
// midiout, which names the MIDI device to send out of, and p1..p8, which give each MIDI port
// a device port of its own. They are not controls, so they are skipped here and read by
// whoever owns them.
class ControlMap {
 public:
  ControlMap();

  // The built-in defaults: the mixer's faders send Volume, its knob rows Cutoff, Resonance
  // and Pan (centred), the faders under the pads send Expression, and the two master faders
  // send nothing.
  void reset();

  // Applies a config file. On a bad line it stops, returns false and, when errorLine is
  // given, reports the 1-based line; lines before it are kept.
  bool load(const char* text, size_t length, uint16_t* errorLine = NULL);

  // The assignment of a control, by ControlGroup and index. Unknown controls send nothing.
  const CcAssignment& assignment(uint8_t group, uint8_t index) const;
  // The 7-bit value a control position, 0..kFaderMax, sends.
  static uint8_t ccValue(const CcAssignment& assignment, uint16_t position);
  // Flat index of a control, 0..kNumCcControls-1, for tables kept alongside the map.
  static uint8_t slotIndex(uint8_t group, uint8_t index);

 private:
  bool applyLine(const char* line, size_t length);
  static bool parseName(const char* name, size_t length, uint8_t& group, uint8_t& firstIndex,
                        uint8_t& count);
  static bool parseValue(const char* value, size_t length, CcAssignment& out);
  CcAssignment* slot(uint8_t group, uint8_t index);

  CcAssignment faders_[kNumTrackFaders];
  CcAssignment master_;
  CcAssignment mixFaders_[kNumMixStrips];
  CcAssignment mixMaster_;
  CcAssignment knobs_[kNumKnobs];
  CcAssignment none_;  // for controls that have no assignment
};

// Controls that can send a CC: the faders under the pads, the master, the mixer's faders,
// its master and its knobs, in that order.
static const uint8_t kNumCcControls =
    kNumTrackFaders + 1 + kNumMixStrips + 1 + kNumKnobs;
static const uint8_t kNoCcSlot = 0xFF;

}  // namespace gx
