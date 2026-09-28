#include "audio/AudioEngine.h"

#include <cmath>

namespace gx {

namespace {

const uint8_t kNoNote = 0xFF;
const float kRampSeconds = 0.004f;  // short enough to feel instant, long enough not to click
// Sixteen slots share the output, so one voice stays well below full scale.
const float kVoiceLevel = 0.18f;
const float kConcertA = 440.0f;
const uint8_t kConcertANote = 69;

float frequencyOf(uint8_t note) {
  return kConcertA * std::pow(2.0f, (static_cast<float>(note) - kConcertANote) / 12.0f);
}

}  // namespace

AudioEngine::AudioEngine()
    : dropped_(0), playing_(0), sampleRate_(48000), rampCoefficient_(1.0f) {
  for (uint8_t i = 0; i < kNumInstruments; ++i) {
    voices_[i].phase = 0.0f;
    voices_[i].increment = 0.0f;
    voices_[i].amplitude = 0.0f;
    voices_[i].target = 0.0f;
    voices_[i].note = kNoNote;
    voices_[i].held = false;
    gains_[i] = 1.0f;
    pans_[i] = 0.0f;
    renderers_[i].store(NULL, std::memory_order_relaxed);
  }
}

void AudioEngine::setSlotRenderer(uint8_t instrument, SlotRenderer* renderer) {
  if (instrument < kNumInstruments) renderers_[instrument].store(renderer, std::memory_order_release);
}

// ---- The sequencer's side ----

void AudioEngine::push(const Event& event) {
  if (event.slot >= kNumInstruments) return;
  if (!queue_.push(event)) dropped_.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) {
  const Event event = {kEventNoteOn, instrument, note, velocity};
  push(event);
}

void AudioEngine::noteOff(uint8_t instrument, uint8_t note) {
  const Event event = {kEventNoteOff, instrument, note, 0};
  push(event);
}

void AudioEngine::controlChange(uint8_t instrument, uint8_t cc, uint8_t value) {
  const Event event = {kEventControl, instrument, cc, value};
  push(event);
}

void AudioEngine::presetChanged(uint8_t instrument, uint16_t preset) {
  // Nothing to pick from yet; the plugin's own presets will land here.
  const Event event = {kEventPreset, instrument, static_cast<uint8_t>(preset & 0x7F), 0};
  push(event);
}

void AudioEngine::setGain(uint8_t instrument, float gain) {
  if (instrument >= kNumInstruments) return;
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  gains_[instrument] = gain;  // one float, written by one thread: no tearing worth guarding
}

void AudioEngine::setPan(uint8_t instrument, float pan) {
  if (instrument >= kNumInstruments) return;
  if (pan < -1.0f) pan = -1.0f;
  if (pan > 1.0f) pan = 1.0f;
  pans_[instrument] = pan;
}

float AudioEngine::gain(uint8_t instrument) const {
  return instrument < kNumInstruments ? gains_[instrument] : 0.0f;
}

float AudioEngine::pan(uint8_t instrument) const {
  return instrument < kNumInstruments ? pans_[instrument] : 0.0f;
}

// ---- The audio thread's side ----

void AudioEngine::prepare(uint32_t sampleRate, uint32_t maxBlockFrames) {
  sampleRate_ = sampleRate ? sampleRate : 48000;
  slotLeft_.assign(maxBlockFrames ? maxBlockFrames : 1024, 0.0f);
  slotRight_.assign(slotLeft_.size(), 0.0f);
  rampCoefficient_ = 1.0f - std::exp(-1.0f / (kRampSeconds * static_cast<float>(sampleRate_)));
  for (uint8_t i = 0; i < kNumInstruments; ++i) {
    voices_[i].amplitude = 0.0f;
    voices_[i].target = 0.0f;
    voices_[i].note = kNoNote;
    voices_[i].held = false;
  }
}

void AudioEngine::applyEvent(const Event& event) {
  Voice& voice = voices_[event.slot];
  SlotRenderer* renderer = renderers_[event.slot].load(std::memory_order_acquire);
  // A plugin gets the notes and every CC except the two the mixer stage keeps for itself.
  if (renderer) {
    switch (event.type) {
      case kEventNoteOn:
        renderer->noteOn(event.a, event.b);
        break;
      case kEventNoteOff:
        renderer->noteOff(event.a);
        break;
      case kEventControl:
        if (event.a != 7 && event.a != 10) renderer->controlChange(event.a, event.b);
        break;
      default:
        break;
    }
  }
  switch (event.type) {
    case kEventNoteOn:
      voice.note = event.a;
      voice.held = true;
      voice.increment = frequencyOf(event.a) / static_cast<float>(sampleRate_);
      voice.target = kVoiceLevel * (static_cast<float>(event.b) / 127.0f);
      break;
    case kEventNoteOff:
      // Only the note that is sounding turns it off, so an overlapping note keeps playing.
      if (voice.held && voice.note == event.a) {
        voice.held = false;
        voice.target = 0.0f;
      }
      break;
    case kEventControl:
      // The two CCs the mixer sends by default: level on the faders, pan on a knob row.
      if (event.a == 7) setGain(event.slot, static_cast<float>(event.b) / 127.0f);
      if (event.a == 10) setPan(event.slot, (static_cast<float>(event.b) - 64.0f) / 63.0f);
      break;
    case kEventPreset:
      break;  // waiting for plugins
  }
}

void AudioEngine::render(float* left, float* right, uint32_t frames) {
  for (uint8_t slot = 0; slot < kNumInstruments; ++slot) {
    SlotRenderer* renderer = renderers_[slot].load(std::memory_order_acquire);
    if (renderer) renderer->beginBlock();  // ready for this block's events
  }

  Event event;
  while (queue_.pop(event)) applyEvent(event);  // everything pending lands at the block start

  for (uint32_t i = 0; i < frames; ++i) {
    left[i] = 0.0f;
    right[i] = 0.0f;
  }

  uint32_t playing = 0;
  for (uint8_t slot = 0; slot < kNumInstruments; ++slot) {
    // Constant power either side, so moving across the image doesn't change how loud it is.
    const float gain = gains_[slot];
    const float angle = (pans_[slot] + 1.0f) * 0.7853981f;  // -1..1 -> 0..pi/2
    const float leftGain = std::cos(angle) * gain;
    const float rightGain = std::sin(angle) * gain;

    SlotRenderer* renderer = renderers_[slot].load(std::memory_order_acquire);
    if (renderer) {
      if (frames > slotLeft_.size()) continue;  // a block bigger than we prepared for
      for (uint32_t i = 0; i < frames; ++i) {
        slotLeft_[i] = 0.0f;
        slotRight_[i] = 0.0f;
      }
      renderer->render(&slotLeft_[0], &slotRight_[0], frames);
      for (uint32_t i = 0; i < frames; ++i) {
        left[i] += slotLeft_[i] * leftGain;
        right[i] += slotRight_[i] * rightGain;
      }
      ++playing;
      continue;
    }

    Voice& voice = voices_[slot];
    if (voice.amplitude <= 0.0000001f && voice.target <= 0.0f) continue;
    ++playing;
    for (uint32_t i = 0; i < frames; ++i) {
      voice.amplitude += (voice.target - voice.amplitude) * rampCoefficient_;
      const float sample = std::sin(voice.phase * 6.2831853f) * voice.amplitude;
      left[i] += sample * leftGain;
      right[i] += sample * rightGain;
      voice.phase += voice.increment;
      if (voice.phase >= 1.0f) voice.phase -= 1.0f;
    }
    if (!voice.held && voice.amplitude < 0.0001f) {
      voice.amplitude = 0.0f;  // fully faded: stop mixing it next block
      voice.note = kNoNote;
    }
  }
  playing_.store(playing, std::memory_order_relaxed);
}

}  // namespace gx
