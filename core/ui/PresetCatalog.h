#pragma once

#include <stdint.h>

namespace gx {

// How many voices the gear on a MIDI port actually has, so preset mode can show the pads and
// windows that lead somewhere and darken the ones that don't.
//
// Implemented outside ui/ (by the platform), like DeviceStatus and Library: the UI never
// learns what a voice list is made of, only how long it is. A build that leaves it unset
// offers the whole grid, which is what an instrument with no lists to read wants anyway.
class PresetCatalog {
 public:
  // The number of preset slots port P1..P8 offers, counting from slot 0.
  virtual uint16_t presetCount(uint8_t port) const = 0;

 protected:
  ~PresetCatalog() {}  // see EventSink: held by pointer, never owned
};

}  // namespace gx
