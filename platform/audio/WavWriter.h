#pragma once

#include <cstdio>
#include <stdint.h>
#include <string>

namespace gx {

// Writes a 16-bit stereo WAV, so an offline render can be listened to or compared. Kept here
// rather than pulling in libsndfile: the header is 44 bytes and we only ever write one shape.
class WavWriter {
 public:
  WavWriter();
  ~WavWriter();

  bool open(const std::string& path, uint32_t sampleRate);
  // Appends one block. Samples outside -1..1 are clipped.
  void write(const float* left, const float* right, uint32_t frames);
  // Fills in the sizes in the header and closes. Returns false if anything went wrong.
  bool close();

  uint32_t framesWritten() const { return frames_; }

 private:
  std::FILE* file_;
  uint32_t sampleRate_;
  uint32_t frames_;
  bool failed_;
};

}  // namespace gx
