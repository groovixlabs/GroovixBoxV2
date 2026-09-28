#pragma once

#include <stdint.h>

#include <atomic>
#include <functional>
#include <thread>

namespace gx {

// Calls a function at a steady rate on its own thread, e.g. GroovixApp::tick, so playback
// timing doesn't depend on how often the window draws. It asks the OS for real-time
// scheduling where it can (Linux SCHED_FIFO) and carries on at normal priority if refused.
// Whatever the function touches must be shared with other threads through a lock.
class ClockThread {
 public:
  explicit ClockThread(std::function<void(uint32_t nowUs)> tick, unsigned periodUs = 1000);
  ~ClockThread();  // stops the thread
  ClockThread(const ClockThread&) = delete;
  ClockThread& operator=(const ClockThread&) = delete;

  void start();
  void stop();
  // Whether the thread got real-time priority. Valid after start().
  bool realTime() const { return realTime_; }

  // The monotonic clock the thread uses. Microseconds wrap about every 71 minutes and
  // milliseconds about every 49 days; the core expects that.
  static uint32_t nowUs();
  static uint32_t nowMs();

 private:
  void run();

  std::function<void(uint32_t)> tick_;
  unsigned periodUs_;
  std::atomic<bool> running_;
  bool realTime_;
  std::thread thread_;
};

}  // namespace gx
