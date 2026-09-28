#pragma once

#include <atomic>
#include <stdint.h>

namespace gx {

// A wait-free queue for one writer and one reader, which is exactly the shape of the
// sequencer thread handing events to the audio thread. Storage is fixed at compile time, so
// pushing and popping never allocate or block. Capacity must be a power of two.
template <typename T, uint32_t kCapacity>
class SpscQueue {
 public:
  SpscQueue() : head_(0), tail_(0) {
    static_assert(kCapacity >= 2 && (kCapacity & (kCapacity - 1)) == 0,
                  "capacity must be a power of two");
  }

  // Writer side. False when the queue is full, which the caller should count rather than
  // retry: the audio thread is behind and blocking it would be worse.
  bool push(const T& item) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) & (kCapacity - 1);
    if (next == tail_.load(std::memory_order_acquire)) return false;
    items_[head] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Reader side. False when there is nothing waiting.
  bool pop(T& item) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    item = items_[tail];
    tail_.store((tail + 1) & (kCapacity - 1), std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }

 private:
  T items_[kCapacity];
  std::atomic<uint32_t> head_;  // written by the producer
  std::atomic<uint32_t> tail_;  // written by the consumer
};

}  // namespace gx
