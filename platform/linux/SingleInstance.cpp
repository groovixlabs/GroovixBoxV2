#include "linux/SingleInstance.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace gx {

SingleInstance::~SingleInstance() {
  if (fd_ >= 0) close(fd_);  // the lock goes with the descriptor
}

bool SingleInstance::take(const std::string& path, long& otherPid) {
  otherPid = 0;
  fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) return true;  // nowhere to put a lock: let it run rather than refuse
  if (flock(fd_, LOCK_EX | LOCK_NB) == 0) {
    if (ftruncate(fd_, 0) == 0) {
      char line[32];
      const int length = std::snprintf(line, sizeof(line), "%ld\n", (long)getpid());
      if (write(fd_, line, static_cast<size_t>(length)) != length) { /* nothing to do */ }
    }
    return true;
  }
  char line[32] = {0};
  const ssize_t readBytes = pread(fd_, line, sizeof(line) - 1, 0);
  if (readBytes > 0) otherPid = std::strtol(line, NULL, 10);
  close(fd_);
  fd_ = -1;
  return false;
}

std::string SingleInstance::devicePath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  return std::string(runtime && *runtime ? runtime : "/tmp") + "/groovix.lock";
}

}  // namespace gx
