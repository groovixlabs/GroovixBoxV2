#include "audio/NullBackend.h"

#include <cstddef>

namespace gx {

NullBackend::NullBackend(uint32_t sampleRate, uint32_t blockSize)
    : renderer_(NULL),
      sampleRate_(sampleRate ? sampleRate : 48000),
      blockSize_(blockSize ? blockSize : 128),
      blocks_(0),
      left_(blockSize_, 0.0f),
      right_(blockSize_, 0.0f) {}

bool NullBackend::start(AudioRenderer& renderer) {
  renderer_ = &renderer;
  renderer_->prepare(sampleRate_, blockSize_);
  return true;
}

void NullBackend::stop() { renderer_ = NULL; }

void NullBackend::renderBlock() {
  if (!renderer_) return;
  renderer_->render(&left_[0], &right_[0], blockSize_);
  ++blocks_;
}

}  // namespace gx
