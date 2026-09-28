#pragma once

#include <vector>

#include "audio/AudioBackend.h"

namespace gx {

// A backend with no device behind it: nothing runs until renderBlock() is called, and the
// samples stay in memory. Tests use it to check what the engine produces, and the offline
// renderer uses it to write a file or to measure how much of a core a set of instruments
// costs — which is how we will size the Raspberry Pi build.
class NullBackend : public AudioBackend {
 public:
  NullBackend(uint32_t sampleRate, uint32_t blockSize);

  bool start(AudioRenderer& renderer) override;
  void stop() override;
  uint32_t sampleRate() const override { return sampleRate_; }
  uint32_t blockSize() const override { return blockSize_; }
  const char* name() const override { return "null"; }

  // Renders one block into left() and right(). Does nothing before start().
  void renderBlock();
  const float* left() const { return &left_[0]; }
  const float* right() const { return &right_[0]; }
  uint32_t blocksRendered() const { return blocks_; }

 private:
  AudioRenderer* renderer_;
  uint32_t sampleRate_;
  uint32_t blockSize_;
  uint32_t blocks_;
  std::vector<float> left_;
  std::vector<float> right_;
};

}  // namespace gx
