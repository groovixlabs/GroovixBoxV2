#pragma once

#include <string>

namespace gx {

// Only one GroovixBox at a time on a machine. Two of them subscribe to the same APC and MIDI
// Mix, so every press reaches both and they fight over the LEDs, and they overwrite each
// other's projects. The lock is a file the kernel releases when the process ends, so a crash
// leaves nothing to clean up.
class SingleInstance {
 public:
  SingleInstance() : fd_(-1) {}
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // Takes the lock. False when another instance holds it, with its pid in otherPid.
  bool take(const std::string& path, long& otherPid);

  // Where the lock for the gear lives: one per machine, not per project.
  static std::string devicePath();

 private:
  int fd_;
};

}  // namespace gx
