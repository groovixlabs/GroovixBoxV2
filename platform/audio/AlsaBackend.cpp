#include "audio/AlsaBackend.h"

#include <pthread.h>

#include <cstdio>

namespace gx {

namespace {

int16_t toPcm(float sample) {
  if (sample > 1.0f) sample = 1.0f;
  if (sample < -1.0f) sample = -1.0f;
  return static_cast<int16_t>(sample * 32767.0f);
}

}  // namespace

AlsaBackend::AlsaBackend(const std::string& device, uint32_t sampleRate, uint32_t blockFrames,
                         uint32_t periods)
    : device_(device),
      sampleRate_(sampleRate ? sampleRate : 48000),
      blockFrames_(blockFrames ? blockFrames : 128),
      periods_(periods < 2 ? 2 : periods),
      pcm_(NULL),
      renderer_(NULL),
      running_(false),
      xruns_(0),
      realTime_(false) {}

AlsaBackend::~AlsaBackend() { stop(); }

bool AlsaBackend::openDevice() {
  int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::fprintf(stderr, "alsa: cannot open %s: %s\n", device_.c_str(), snd_strerror(err));
    pcm_ = NULL;
    return false;
  }

  snd_pcm_hw_params_t* hw = NULL;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm_, hw);
  err = snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err >= 0) err = snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE);
  if (err >= 0) err = snd_pcm_hw_params_set_channels(pcm_, hw, 2);
  if (err >= 0) {
    unsigned int rate = sampleRate_;
    err = snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, NULL);
    sampleRate_ = rate;  // the device may only do a nearby rate
  }
  if (err >= 0) {
    snd_pcm_uframes_t period = blockFrames_;
    err = snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, NULL);
    blockFrames_ = static_cast<uint32_t>(period);
  }
  if (err >= 0) {
    unsigned int periods = periods_;
    err = snd_pcm_hw_params_set_periods_near(pcm_, hw, &periods, NULL);
    periods_ = periods;
  }
  if (err >= 0) err = snd_pcm_hw_params(pcm_, hw);
  if (err < 0) {
    std::fprintf(stderr, "alsa: %s will not take our format: %s\n", device_.c_str(),
                 snd_strerror(err));
    snd_pcm_close(pcm_);
    pcm_ = NULL;
    return false;
  }

  snd_pcm_sw_params_t* sw = NULL;
  snd_pcm_sw_params_alloca(&sw);
  snd_pcm_sw_params_current(pcm_, sw);
  // Start once a full buffer is queued, and wake us as soon as one period is free.
  snd_pcm_sw_params_set_start_threshold(pcm_, sw, blockFrames_ * (periods_ - 1));
  snd_pcm_sw_params_set_avail_min(pcm_, sw, blockFrames_);
  err = snd_pcm_sw_params(pcm_, sw);
  if (err < 0) {
    std::fprintf(stderr, "alsa: cannot set the wake-up points: %s\n", snd_strerror(err));
    snd_pcm_close(pcm_);
    pcm_ = NULL;
    return false;
  }
  return true;
}

bool AlsaBackend::start(AudioRenderer& renderer) {
  if (running_.load()) return true;
  if (!openDevice()) return false;

  renderer_ = &renderer;
  renderer_->prepare(sampleRate_, blockFrames_);
  left_.assign(blockFrames_, 0.0f);
  right_.assign(blockFrames_, 0.0f);
  interleaved_.assign(blockFrames_ * 2, 0);

  running_.store(true);
  thread_ = std::thread(&AlsaBackend::run, this);

  // Audio must outrank everything else on the box, or the buffer runs dry under load. It
  // needs rtprio in /etc/security/limits.d; without it we still play, just less reliably.
  sched_param param;
  param.sched_priority = 70;
  realTime_ = pthread_setschedparam(thread_.native_handle(), SCHED_FIFO, &param) == 0;
  return true;
}

void AlsaBackend::run() {
  while (running_.load(std::memory_order_relaxed)) {
    renderer_->render(&left_[0], &right_[0], blockFrames_);
    for (uint32_t i = 0; i < blockFrames_; ++i) {
      interleaved_[2 * i] = toPcm(left_[i]);
      interleaved_[2 * i + 1] = toPcm(right_[i]);
    }

    const uint8_t* from = reinterpret_cast<const uint8_t*>(&interleaved_[0]);
    snd_pcm_uframes_t left = blockFrames_;
    while (left > 0 && running_.load(std::memory_order_relaxed)) {
      const snd_pcm_sframes_t wrote = snd_pcm_writei(pcm_, from, left);
      if (wrote >= 0) {
        left -= static_cast<snd_pcm_uframes_t>(wrote);
        from += static_cast<size_t>(wrote) * 2 * sizeof(int16_t);
        continue;
      }
      if (wrote == -EAGAIN) continue;
      // Underrun or a suspended device: count it and carry on rather than dying mid-set.
      xruns_.fetch_add(1, std::memory_order_relaxed);
      if (snd_pcm_recover(pcm_, static_cast<int>(wrote), 1) < 0) return;
    }
  }
}

void AlsaBackend::stop() {
  if (running_.exchange(false)) {
    if (thread_.joinable()) thread_.join();
  }
  if (pcm_) {
    snd_pcm_drop(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = NULL;
  }
  renderer_ = NULL;
}

}  // namespace gx
