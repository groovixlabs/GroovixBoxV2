#pragma once

#include <stdint.h>

#include "ui/Controls.h"
#include "ui/Rgb.h"

namespace gx {

// What a screen beside the instrument shows. Filled by the UI, drawn by the platform - the
// same division as LedFrame and ControlSurface, so core carries no fonts and no pixels, and a
// build with no screen simply never asks for one.
//
// It holds values and pointers to string literals, never formatted text: core has no printf
// (see cmake/check_layers.cmake - only stdint, stddef and string.h are portable everywhere),
// so turning a number into digits is the platform's job.
//
// There are two things worth showing, and which one depends on whether the sequencer is
// running. While it plays you want to know what every track is doing; while it is stopped you
// are editing, and what helps is what this mode's rows are for. Only the middle of the screen
// changes between them - the transport above and the values below stay where they are, so the
// screen never rearranges itself under you.

// One line of a mode's legend: a band of rows, or a modifier worth naming.
struct DisplayBand {
  uint8_t firstRow;   // 1-based; 0 for a band that is a modifier rather than rows
  uint8_t lastRow;    // 1-based, inclusive
  Rgb color;          // the colour those rows light, for the chip beside the line
  const char* label;  // "rows 1-4", "SHIFT 7-8"
  const char* text;   // what they do
};

// A value worth spelling out: either a number or a word, never both.
struct DisplayValue {
  const char* key;    // "VELOCITY"
  const char* text;   // the value as a word ("1/2 step", "off"), or NULL to use `number`
  uint16_t number;
  const char* suffix; // " step", " steps", or NULL - drawn smaller after the number
};

static const uint8_t kMaxDisplayBands = 6;
static const uint8_t kMaxDisplayMods = 4;
static const uint8_t kMaxDisplayValues = 5;

// What a mode says about itself. All of it is static: a mode returns a pointer to its own
// constant, and nothing is copied or built at runtime.
struct ModeLegend {
  const char* key;   // the button that opens it: "R4", "SHIFT + R5"
  const char* name;  // "Step parameters"
  DisplayBand bands[kMaxDisplayBands];
  uint8_t numBands;
  const char* mods[kMaxDisplayMods];  // "hold R4 + B1-B8 step page"
  uint8_t numMods;
  Rgb rowColor[kGridRows];  // the miniature grid drawn beside the lines
};

// Where a track's notes go, for the route column.
struct DisplayTrack {
  Rgb color;
  uint8_t number;      // 1-based, as the surface labels it
  uint8_t port;        // 0-based MIDI port, or kNoDisplayPort when on an instrument
  uint8_t channel;     // 0-based MIDI channel
  uint8_t instrument;  // 0-based slot, or kNoDisplayPort when on a MIDI port
  uint16_t preset;     // the slot: the platform names it from the device's voice list
  uint8_t pattern;     // 1-based
  bool muted;
  bool soloed;
  uint8_t activity;    // 0..255, how much this track has played lately
};

static const uint8_t kNoDisplayPort = 0xFF;

struct DisplayFrame {
  // ---- the top band, the same whatever is happening ----
  uint16_t bpm;
  uint8_t clockSource;       // kClockInternal / kClockAuto / kClockExternal
  bool followingExternal;    // an outside clock is really driving
  bool playing;
  bool recording;

  // ---- the middle ----
  bool showTracks;  // playing: the track page. Stopped: the legend below.
  DisplayTrack tracks[kNumBottomButtons];
  uint8_t numTracks;

  const ModeLegend* legend;  // NULL for a mode that has nothing to say
  // The legend header's right side, as numbers for the platform to spell: "TRACK 3 - PAT 2 -
  // STEPS 193-224". Each is 1-based and left out when zero, so a mode with no steps on the
  // grid simply says which track it is on.
  uint16_t contextTrack;
  uint16_t contextPattern;
  uint16_t contextFirstStep;
  uint16_t contextLastStep;

  // ---- the bottom band ----
  DisplayValue values[kMaxDisplayValues];
  uint8_t numValues;
  uint16_t project;  // the open project slot, shown page.pad
};

// A screen. The platform implements it; the UI never learns what a pixel is.
class DisplaySurface {
 public:
  virtual void show(const DisplayFrame& frame) = 0;

 protected:
  ~DisplaySurface() {}  // see EventSink: held by pointer, never owned
};

}  // namespace gx
