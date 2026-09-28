#pragma once

#include <stdint.h>

namespace gx {

// What an instrument slot's sound comes from, when it isn't the engine's own stand-in tone.
// An LV2 plugin implements this; the engine calls it from the audio thread only, in this
// order every block: beginBlock(), then any notes and CCs that arrived, then render().
class SlotRenderer {
 public:
  virtual void beginBlock() = 0;
  virtual void noteOn(uint8_t note, uint8_t velocity) = 0;
  virtual void noteOff(uint8_t note) = 0;
  virtual void controlChange(uint8_t cc, uint8_t value) = 0;
  // Adds its output into the two buffers; the engine applies the slot's level and pan.
  virtual void render(float* left, float* right, uint32_t frames) = 0;

 protected:
  ~SlotRenderer() {}
};

}  // namespace gx
