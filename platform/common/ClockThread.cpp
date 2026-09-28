#include "common/ClockThread.h"

#include <chrono>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace gx {

namespace {

typedef std::chrono::steady_clock Clock;

const int kRealTimePriorityAboveMin = 10;  // modest: above normal work, below system threads

}  // namespace

ClockThread::ClockThread(std::function<void(uint32_t nowUs)> tick, unsigned periodUs)
    : tick_(tick), periodUs_(periodUs), running_(false), realTime_(false) {}

ClockThread::~ClockThread() { stop(); }

uint32_t ClockThread::nowUs() {
  const auto us =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch());
  return static_cast<uint32_t>(us.count());
}

uint32_t ClockThread::nowMs() {
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch());
  return static_cast<uint32_t>(ms.count());
}

void ClockThread::start() {
  if (running_) return;
  running_ = true;
  thread_ = std::thread([this] { run(); });
#ifdef __linux__
  sched_param param;
  param.sched_priority = sched_get_priority_min(SCHED_FIFO) + kRealTimePriorityAboveMin;
  realTime_ = pthread_setschedparam(thread_.native_handle(), SCHED_FIFO, &param) == 0;
#endif
}

void ClockThread::stop() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void ClockThread::run() {
  const std::chrono::microseconds period(periodUs_);
  Clock::time_point next = Clock::now();
  while (running_) {
    tick_(nowUs());
    // Sleep until the next tick on a fixed grid, so small delays don't add up. After a long
    // stall, skip ahead rather than catch up in a burst: the sequencer handles the gap.
    next += period;
    const Clock::time_point now = Clock::now();
    if (next < now) next = now;
    std::this_thread::sleep_until(next);
  }
}

}  // namespace gx
