#include "audio/WavWriter.h"

#include <cstring>

namespace gx {

namespace {

const uint32_t kHeaderSize = 44;
const uint16_t kChannels = 2;
const uint16_t kBitsPerSample = 16;

void putU32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void putU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value & 0xFF);
  p[1] = static_cast<uint8_t>(value >> 8);
}

int16_t toPcm(float sample) {
  if (sample > 1.0f) sample = 1.0f;
  if (sample < -1.0f) sample = -1.0f;
  return static_cast<int16_t>(sample * 32767.0f);
}

}  // namespace

WavWriter::WavWriter() : file_(NULL), sampleRate_(48000), frames_(0), failed_(false) {}

WavWriter::~WavWriter() {
  if (file_) std::fclose(file_);
}

bool WavWriter::open(const std::string& path, uint32_t sampleRate) {
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) return false;
  sampleRate_ = sampleRate;
  frames_ = 0;
  failed_ = false;
  uint8_t header[kHeaderSize];
  std::memset(header, 0, sizeof(header));
  std::fwrite(header, 1, sizeof(header), file_);  // sizes are filled in by close()
  return true;
}

void WavWriter::write(const float* left, const float* right, uint32_t frames) {
  if (!file_ || failed_) return;
  for (uint32_t i = 0; i < frames; ++i) {
    uint8_t frame[4];
    putU16(frame, static_cast<uint16_t>(toPcm(left[i])));
    putU16(frame + 2, static_cast<uint16_t>(toPcm(right[i])));
    if (std::fwrite(frame, 1, sizeof(frame), file_) != sizeof(frame)) {
      failed_ = true;
      return;
    }
  }
  frames_ += frames;
}

bool WavWriter::close() {
  if (!file_) return false;
  const uint32_t dataBytes = frames_ * kChannels * (kBitsPerSample / 8);
  const uint32_t byteRate = sampleRate_ * kChannels * (kBitsPerSample / 8);

  uint8_t header[kHeaderSize];
  std::memcpy(header, "RIFF", 4);
  putU32(header + 4, 36 + dataBytes);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  putU32(header + 16, 16);                 // PCM chunk size
  putU16(header + 20, 1);                  // PCM
  putU16(header + 22, kChannels);
  putU32(header + 24, sampleRate_);
  putU32(header + 28, byteRate);
  putU16(header + 32, kChannels * (kBitsPerSample / 8));  // block align
  putU16(header + 34, kBitsPerSample);
  std::memcpy(header + 36, "data", 4);
  putU32(header + 40, dataBytes);

  const bool seeked = std::fseek(file_, 0, SEEK_SET) == 0;
  const bool wrote = seeked && std::fwrite(header, 1, sizeof(header), file_) == sizeof(header);
  const bool closed = std::fclose(file_) == 0;
  file_ = NULL;
  return wrote && closed && !failed_;
}

}  // namespace gx
