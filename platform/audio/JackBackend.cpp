#include "audio/JackBackend.h"

#include <cstdio>
#include <cstring>

namespace gx {

JackBackend::JackBackend(const std::string& clientName, bool autoConnect)
    : clientName_(clientName),
      autoConnect_(autoConnect),
      client_(NULL),
      leftPort_(NULL),
      rightPort_(NULL),
      renderer_(NULL),
      xruns_(0),
      sampleRate_(48000),
      blockSize_(128) {}

JackBackend::~JackBackend() { stop(); }

int JackBackend::processCallback(jack_nframes_t frames, void* self) {
  JackBackend* backend = static_cast<JackBackend*>(self);
  float* left = static_cast<float*>(jack_port_get_buffer(backend->leftPort_, frames));
  float* right = static_cast<float*>(jack_port_get_buffer(backend->rightPort_, frames));
  if (backend->renderer_) {
    backend->renderer_->render(left, right, frames);
  } else {
    std::memset(left, 0, frames * sizeof(float));
    std::memset(right, 0, frames * sizeof(float));
  }
  return 0;
}

int JackBackend::blockSizeCallback(jack_nframes_t frames, void* self) {
  JackBackend* backend = static_cast<JackBackend*>(self);
  backend->blockSize_ = frames;
  if (backend->renderer_) backend->renderer_->prepare(backend->sampleRate_, frames);
  return 0;
}

int JackBackend::sampleRateCallback(jack_nframes_t rate, void* self) {
  JackBackend* backend = static_cast<JackBackend*>(self);
  backend->sampleRate_ = rate;
  if (backend->renderer_) backend->renderer_->prepare(rate, backend->blockSize_);
  return 0;
}

int JackBackend::xrunCallback(void* self) {
  static_cast<JackBackend*>(self)->xruns_.fetch_add(1, std::memory_order_relaxed);
  return 0;
}

bool JackBackend::start(AudioRenderer& renderer) {
  if (client_) return true;

  jack_status_t status = JackFailure;
  client_ = jack_client_open(clientName_.c_str(), JackNoStartServer, &status);
  if (!client_) {
    std::fprintf(stderr, "jack: no server (status 0x%x)\n", static_cast<unsigned>(status));
    return false;
  }

  sampleRate_ = jack_get_sample_rate(client_);
  blockSize_ = jack_get_buffer_size(client_);
  renderer_ = &renderer;
  renderer_->prepare(sampleRate_, blockSize_);

  leftPort_ = jack_port_register(client_, "out_left", JACK_DEFAULT_AUDIO_TYPE,
                                 JackPortIsOutput, 0);
  rightPort_ = jack_port_register(client_, "out_right", JACK_DEFAULT_AUDIO_TYPE,
                                  JackPortIsOutput, 0);
  if (!leftPort_ || !rightPort_) {
    std::fprintf(stderr, "jack: cannot register ports\n");
    stop();
    return false;
  }

  jack_set_process_callback(client_, processCallback, this);
  jack_set_buffer_size_callback(client_, blockSizeCallback, this);
  jack_set_sample_rate_callback(client_, sampleRateCallback, this);
  jack_set_xrun_callback(client_, xrunCallback, this);

  if (jack_activate(client_) != 0) {
    std::fprintf(stderr, "jack: cannot activate the client\n");
    stop();
    return false;
  }
  if (autoConnect_) connectToSpeakers();
  return true;
}

// Wires our two outputs to the first pair of physical playback ports, so sound comes out
// without the user patching anything.
void JackBackend::connectToSpeakers() {
  const char** playback =
      jack_get_ports(client_, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
  if (!playback) {
    std::fprintf(stderr, "jack: no playback ports to connect to\n");
    return;
  }
  if (playback[0]) jack_connect(client_, jack_port_name(leftPort_), playback[0]);
  if (playback[1]) jack_connect(client_, jack_port_name(rightPort_), playback[1]);
  jack_free(playback);
}

void JackBackend::stop() {
  if (!client_) return;
  jack_deactivate(client_);
  jack_client_close(client_);
  client_ = NULL;
  leftPort_ = NULL;
  rightPort_ = NULL;
  renderer_ = NULL;
}

}  // namespace gx
