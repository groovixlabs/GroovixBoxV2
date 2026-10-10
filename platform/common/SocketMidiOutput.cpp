#include "common/SocketMidiOutput.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

namespace gx {

SocketMidiOutput::SocketMidiOutput()
    : fd_(-1), port_(0), sent_(0), dropped_(0), addrLen_(0) {
  memset(addr_, 0, sizeof(addr_));
}

SocketMidiOutput::~SocketMidiOutput() { close(); }

bool SocketMidiOutput::parseTarget(const std::string& text, std::string& host, uint16_t& port) {
  const size_t colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
  const std::string portText = text.substr(colon + 1);
  for (size_t i = 0; i < portText.size(); ++i) {
    if (portText[i] < '0' || portText[i] > '9') return false;
  }
  const long value = std::strtol(portText.c_str(), NULL, 10);
  if (value < 1 || value > 65535) return false;
  host = text.substr(0, colon);
  port = static_cast<uint16_t>(value);
  return true;
}

bool SocketMidiOutput::open(const std::string& host, uint16_t port) {
  close();
  if (host.empty() || port == 0) return false;

  sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  // A dotted address only: inet_pton does not touch the network, where resolving a name
  // could block for seconds on a rig whose DNS is not there.
  if (inet_pton(AF_INET, host.c_str(), &to.sin_addr) != 1) return false;

  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  // Non-blocking as well as connectionless: a full send buffer must return at once rather
  // than wait for room, since the caller is the sequencer's clock.
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(fd);
    return false;
  }

  fd_ = fd;
  host_ = host;
  port_ = port;
  memcpy(addr_, &to, sizeof(to));
  addrLen_ = sizeof(to);
  sent_ = 0;
  dropped_ = 0;
  return true;
}

void SocketMidiOutput::close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  host_.clear();
  port_ = 0;
  addrLen_ = 0;
}

void SocketMidiOutput::send(const MidiMessage& message) {
  if (fd_ < 0) return;
  uint8_t bytes[3];
  const uint8_t length = midiMessageBytes(message, bytes);
  if (length == 0) return;
  const ssize_t written =
      sendto(fd_, bytes, length, MSG_DONTWAIT,
             reinterpret_cast<const sockaddr*>(addr_), static_cast<socklen_t>(addrLen_));
  // Nothing is retried and nothing waits. A server that is not listening makes the kernel
  // report ECONNREFUSED on the *next* send, which is counted and otherwise ignored: the
  // point of a datagram here is that the beat goes on whatever the far end is doing.
  if (written == static_cast<ssize_t>(length)) {
    ++sent_;
  } else {
    ++dropped_;
  }
}

}  // namespace gx
