#pragma once

#include <atomic>
#include <jack/jack.h>
#include <string>

#include "audio/AudioBackend.h"

namespace gx {

// Audio through JACK, which on a modern desktop is usually PipeWire answering the JACK API.
// JACK owns the real-time thread, the block size and the sample rate, so this is the easiest
// way to get low latency while developing; the standalone build uses AlsaBackend instead.
class JackBackend : public AudioBackend {
 public:
  explicit JackBackend(const std::string& clientName = "GroovixBox", bool autoConnect = true);
  ~JackBackend() override;

  bool start(AudioRenderer& renderer) override;
  void stop() override;
  uint32_t sampleRate() const override { return sampleRate_; }
  uint32_t blockSize() const override { return blockSize_; }
  uint32_t xruns() const override { return xruns_.load(std::memory_order_relaxed); }
  const char* name() const override { return "jack"; }

 private:
  static int processCallback(jack_nframes_t frames, void* self);
  static int blockSizeCallback(jack_nframes_t frames, void* self);
  static int sampleRateCallback(jack_nframes_t rate, void* self);
  static int xrunCallback(void* self);
  void connectToSpeakers();

  std::string clientName_;
  bool autoConnect_;
  jack_client_t* client_;
  jack_port_t* leftPort_;
  jack_port_t* rightPort_;
  AudioRenderer* renderer_;
  std::atomic<uint32_t> xruns_;
  uint32_t sampleRate_;
  uint32_t blockSize_;
};

}  // namespace gx
