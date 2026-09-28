#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioBackend.h"

namespace gx {

// Audio straight to an ALSA device, with our own real-time thread. This is what the
// standalone instrument uses: no audio server, no IPC, one less thing in the boot path, and
// on a board with an I2S DAC it is the shortest way to the converters.
//
// It asks for SCHED_FIFO and says whether it got it — without it, xruns are a matter of time.
//
// Pacing comes from the device: the thread blocks in snd_pcm_writei until the card has room.
// Point it at a device that never blocks (ALSA's "null" or "file" plugins) and it will render
// as fast as the CPU allows, which is only useful for testing.
class AlsaBackend : public AudioBackend {
 public:
  AlsaBackend(const std::string& device, uint32_t sampleRate, uint32_t blockFrames,
              uint32_t periods = 3);
  ~AlsaBackend() override;

  bool start(AudioRenderer& renderer) override;
  void stop() override;
  uint32_t sampleRate() const override { return sampleRate_; }
  uint32_t blockSize() const override { return blockFrames_; }
  uint32_t xruns() const override { return xruns_.load(std::memory_order_relaxed); }
  const char* name() const override { return "alsa"; }

  // Whether the audio thread runs at real-time priority.
  bool realTime() const { return realTime_; }

 private:
  bool openDevice();
  void run();  // the audio thread

  std::string device_;
  uint32_t sampleRate_;
  uint32_t blockFrames_;
  uint32_t periods_;
  snd_pcm_t* pcm_;
  AudioRenderer* renderer_;
  std::thread thread_;
  std::atomic<bool> running_;
  std::atomic<uint32_t> xruns_;
  bool realTime_;
  std::vector<float> left_;
  std::vector<float> right_;
  std::vector<int16_t> interleaved_;
};

}  // namespace gx
