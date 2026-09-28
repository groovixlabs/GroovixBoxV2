#pragma once

#include <stdint.h>

namespace gx {

// Renders one block of audio. Called on the audio thread: no allocation, no locks, no file
// or console I/O inside render().
class AudioRenderer {
 public:
  // Called before any render(), and again if the device changes rate or block size.
  virtual void prepare(uint32_t sampleRate, uint32_t maxBlockFrames) = 0;
  // Fills two planar buffers of `frames` samples, overwriting whatever is there.
  virtual void render(float* left, float* right, uint32_t frames) = 0;

 protected:
  ~AudioRenderer() {}
};

// Somewhere audio comes out: JACK, ALSA, CoreAudio, or nothing at all (NullBackend, which
// renders on demand for tests and offline rendering).
class AudioBackend {
 public:
  virtual ~AudioBackend() {}

  // Opens the device and starts calling renderer. Returns false, with a reason printed, if
  // the device can't be opened.
  virtual bool start(AudioRenderer& renderer) = 0;
  virtual void stop() = 0;
  virtual uint32_t sampleRate() const = 0;
  virtual uint32_t blockSize() const = 0;
  // Blocks the device dropped because we were too slow. Zero is the goal.
  virtual uint32_t xruns() const { return 0; }
  virtual const char* name() const = 0;
};

}  // namespace gx
