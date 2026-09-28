#pragma once

#include <atomic>
#include <stdint.h>

#include <vector>

#include "audio/AudioBackend.h"
#include "audio/SlotRenderer.h"
#include "audio/SpscQueue.h"
#include "comm/InstrumentOutput.h"
#include "engine/Project.h"

namespace gx {

// Plays the tracks routed to internal instruments I1..I16.
//
// The sequencer calls the InstrumentOutput side from its own thread; the backend calls
// render() from the audio thread. Nothing is shared between them but a wait-free queue, so
// neither waits for the other and the audio thread never allocates or locks.
//
// A slot with a plugin attached (setSlotRenderer) plays that; the rest fall back to a plain
// sine with a short envelope, which is what proved the path before plugins arrived and what
// CPU measurements were taken against.
class AudioEngine : public InstrumentOutput, public AudioRenderer {
 public:
  AudioEngine();

  // ---- InstrumentOutput: called from the sequencer's thread ----
  void noteOn(uint8_t instrument, uint8_t note, uint8_t velocity) override;
  void noteOff(uint8_t instrument, uint8_t note) override;
  void controlChange(uint8_t instrument, uint8_t cc, uint8_t value) override;
  void presetChanged(uint8_t instrument, uint16_t preset) override;

  // ---- AudioRenderer: called from the audio thread ----
  void prepare(uint32_t sampleRate, uint32_t maxBlockFrames) override;
  void render(float* left, float* right, uint32_t frames) override;

  // What plays a slot, instead of the stand-in tone. Attach before the audio starts; the
  // engine calls it only from the audio thread. NULL puts the slot back to the tone.
  void setSlotRenderer(uint8_t instrument, SlotRenderer* renderer);

  // ---- Diagnostics, safe to read from anywhere ----
  // Events dropped because the audio thread was too far behind. Should stay at zero.
  uint32_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }
  uint32_t voicesPlaying() const { return playing_.load(std::memory_order_relaxed); }
  // Level of an instrument slot, 0..1, as its fader or CC 7 sets it.
  void setGain(uint8_t instrument, float gain);
  // Where it sits between the speakers: -1 hard left, 0 centre, 1 hard right (CC 10).
  void setPan(uint8_t instrument, float pan);
  float gain(uint8_t instrument) const;
  float pan(uint8_t instrument) const;

 private:
  enum EventType { kEventNoteOn, kEventNoteOff, kEventControl, kEventPreset };

  struct Event {
    uint8_t type;
    uint8_t slot;
    uint8_t a;  // note, or CC number
    uint8_t b;  // velocity, or CC value
  };

  // One slot's stand-in voice: monophonic, last note wins.
  struct Voice {
    float phase;      // 0..1
    float increment;  // phase per sample
    float amplitude;  // what it is now, ramped towards target
    float target;     // what it should be
    uint8_t note;     // the note sounding, or kInvalidNote
    bool held;
  };

  void push(const Event& event);
  void applyEvent(const Event& event);

  SpscQueue<Event, 256> queue_;
  std::atomic<uint32_t> dropped_;
  std::atomic<uint32_t> playing_;
  Voice voices_[kNumInstruments];
  std::atomic<SlotRenderer*> renderers_[kNumInstruments];
  float gains_[kNumInstruments];
  float pans_[kNumInstruments];
  std::vector<float> slotLeft_;   // one slot's output, before its level and pan
  std::vector<float> slotRight_;
  uint32_t sampleRate_;
  float rampCoefficient_;  // per-sample approach to the target amplitude
};

}  // namespace gx
