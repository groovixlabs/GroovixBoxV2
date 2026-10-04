#include "linux/AlsaMidiOutput.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace gx {

namespace {

static_assert(kNumMidiPorts <= 8, "one bit per port fits in connected_");

const unsigned kSourceCaps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
const unsigned kSinkCaps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
// Sends come from the clock thread, so a full queue is waited out briefly and no longer:
// a late note is better than a missing one, but not at the cost of the beat.
const int kSendAttempts = 10;
const useconds_t kSendRetryUs = 500;

// Bytes in a channel or realtime message, 0 for one we don't send (SysEx and friends).
uint8_t messageLength(uint8_t status) {
  if (status >= 0xF8) return 1;  // clock, start, stop and the rest of the realtime bytes
  if (status >= 0xF0) return 0;  // system common: not sent through MidiMessage
  switch (status & 0xF0) {
    case 0xC0:  // program change
    case 0xD0:  // channel pressure
      return 2;
    default:
      return 3;
  }
}

}  // namespace

AlsaMidiOutput::AlsaMidiOutput() : seq_(NULL), connected_(0), dropped_(0) {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) ports_[port] = -1;
}

AlsaMidiOutput::~AlsaMidiOutput() {
  if (seq_) snd_seq_close(seq_);
}

bool AlsaMidiOutput::open(const char* clientName) {
  if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) {
    seq_ = NULL;
    return false;
  }
  snd_seq_set_client_name(seq_, clientName);
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    char name[8];
    std::snprintf(name, sizeof(name), "P%u", static_cast<unsigned>(port) + 1);
    ports_[port] = snd_seq_create_simple_port(
        seq_, name, kSourceCaps,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (ports_[port] < 0) return false;
  }
  return true;
}

const std::string& AlsaMidiOutput::portDeviceName(uint8_t port) const {
  static const std::string kNone;
  return port < kNumMidiPorts ? portDevice_[port] : kNone;
}

void AlsaMidiOutput::forgetPortDevices() {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) portDevice_[port].clear();
}

// The device's inputs, in the order it lists them: P1 to the first, P2 to the second...
uint8_t AlsaMidiOutput::connectTo(int client, const char* name) {
  snd_seq_port_info_t* info = NULL;
  snd_seq_port_info_alloca(&info);
  uint8_t port = 0;
  snd_seq_port_info_set_client(info, client);
  snd_seq_port_info_set_port(info, -1);
  while (port < kNumMidiPorts && snd_seq_query_next_port(seq_, info) >= 0) {
    if ((snd_seq_port_info_get_capability(info) & kSinkCaps) != kSinkCaps) continue;
    if (snd_seq_connect_to(seq_, ports_[port], client, snd_seq_port_info_get_port(info)) < 0) {
      continue;
    }
    connected_ = static_cast<uint8_t>(connected_ | (1u << port));
    if (name) portDevice_[port] = name;
    ++port;
  }
  if (port > 0 && name) deviceName_ = name;
  return port;
}

uint8_t AlsaMidiOutput::connectDevice(const char* clientNameContains) {
  if (!seq_ || !clientNameContains) return 0;
  snd_seq_client_info_t* client = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    const char* name = snd_seq_client_info_get_name(client);
    if (!name || !std::strstr(name, clientNameContains)) continue;
    return connectTo(id, name);
  }
  return 0;
}

// Clients that answer to the description of a MIDI interface but are not one: the kernel's
// loopback, and the control surfaces we drive ourselves.
static bool isNotAnInterface(const char* name) {
  static const char* const kSkip[] = {"Midi Through", "APC mini", "MIDI Mix", "Launchpad"};
  for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
    if (std::strstr(name, kSkip[i])) return true;
  }
  return false;
}

// Part of a name, ignoring case: config files are written by hand, and "digitone" should
// find "Elektron Digitone".
static bool containsNoCase(const char* haystack, const char* needle, size_t needleLength) {
  if (needleLength == 0) return true;
  for (size_t at = 0; haystack[at] != '\0'; ++at) {
    size_t i = 0;
    while (i < needleLength && haystack[at + i] != '\0' &&
           std::tolower(static_cast<unsigned char>(haystack[at + i])) ==
               std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == needleLength) return true;
  }
  return false;
}

// Connects our port to a device port and says what it connected to, or "" if it could not.
std::string AlsaMidiOutput::connectHere(uint8_t port, int client, const char* clientName,
                                        snd_seq_port_info_t* info) {
  if (snd_seq_connect_to(seq_, ports_[port], client, snd_seq_port_info_get_port(info)) < 0) {
    return std::string();
  }
  connected_ = static_cast<uint8_t>(connected_ | (1u << port));
  portDevice_[port] = clientName;
  if (deviceName_.empty()) deviceName_ = clientName;
  // ALSA port names usually repeat the device's, so "MIDI4x4 Midi Out 3" needs saying once
  // rather than twice.
  const char* portName = snd_seq_port_info_get_name(info);
  if (!portName) return std::string(clientName);
  if (containsNoCase(portName, clientName, std::strlen(clientName))) return std::string(portName);
  return std::string(clientName) + " " + portName;
}

std::string AlsaMidiOutput::connectPortTo(uint8_t port, const char* spec) {
  if (!seq_ || port >= kNumMidiPorts || ports_[port] < 0 || !spec) return std::string();

  // "device" or "device:which", where which is a number (the nth input it offers) or part of
  // the input's name. The last colon splits them, since a device name rarely holds one.
  std::string text(spec);
  std::string device = text;
  std::string which;
  const size_t colon = text.rfind(':');
  if (colon != std::string::npos) {
    device = text.substr(0, colon);
    which = text.substr(colon + 1);
  }
  while (!device.empty() && std::isspace(static_cast<unsigned char>(device.back()))) device.pop_back();
  size_t start = 0;
  while (start < which.size() && std::isspace(static_cast<unsigned char>(which[start]))) ++start;
  which = which.substr(start);
  while (!which.empty() && std::isspace(static_cast<unsigned char>(which.back()))) which.pop_back();

  int ordinal = 0;  // 1-based, 0 when the port is named instead
  if (!which.empty()) {
    bool digits = true;
    for (size_t i = 0; i < which.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(which[i]))) digits = false;
    }
    if (digits) ordinal = std::atoi(which.c_str());
  }
  if (which.empty()) ordinal = 1;  // no port given: the device's first input

  snd_seq_client_info_t* client = NULL;
  snd_seq_port_info_t* info = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_alloca(&info);
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    const char* clientName = snd_seq_client_info_get_name(client);
    if (!clientName || !containsNoCase(clientName, device.c_str(), device.size())) continue;

    int seen = 0;
    snd_seq_port_info_set_client(info, id);
    snd_seq_port_info_set_port(info, -1);
    while (snd_seq_query_next_port(seq_, info) >= 0) {
      if ((snd_seq_port_info_get_capability(info) & kSinkCaps) != kSinkCaps) continue;
      const char* portName = snd_seq_port_info_get_name(info);
      ++seen;
      const bool match = ordinal > 0 ? seen == ordinal
                                     : (portName && containsNoCase(portName, which.c_str(),
                                                                   which.size()));
      if (!match) continue;
      return connectHere(port, id, clientName, info);
    }
  }

  // Nothing answered to that as a device. A port listing prints the port's own name, which
  // usually carries the device's inside it - "MIDI4x4 Midi Out 2" - so try the text against
  // the ports themselves, which is what someone copying a line out of aconnect will have
  // written. Only without a colon: with one, the device was named on purpose.
  if (colon != std::string::npos) return std::string();
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    const char* clientName = snd_seq_client_info_get_name(client);
    if (!clientName) continue;
    snd_seq_port_info_set_client(info, id);
    snd_seq_port_info_set_port(info, -1);
    while (snd_seq_query_next_port(seq_, info) >= 0) {
      if ((snd_seq_port_info_get_capability(info) & kSinkCaps) != kSinkCaps) continue;
      const char* portName = snd_seq_port_info_get_name(info);
      if (!portName || !containsNoCase(portName, device.c_str(), device.size())) continue;
      return connectHere(port, id, clientName, info);
    }
  }
  return std::string();
}

// How many inputs a client offers, which is how an interface is told from a controller: a
// 4x4 has four, a keyboard has the one it lights its pads through.
uint8_t AlsaMidiOutput::countSinkPorts(int client) const {
  snd_seq_port_info_t* info = NULL;
  snd_seq_port_info_alloca(&info);
  snd_seq_port_info_set_client(info, client);
  snd_seq_port_info_set_port(info, -1);
  uint8_t ports = 0;
  while (snd_seq_query_next_port(seq_, info) >= 0) {
    if ((snd_seq_port_info_get_capability(info) & kSinkCaps) == kSinkCaps) ++ports;
  }
  return ports;
}

uint8_t AlsaMidiOutput::connectFirstInterface() {
  if (!seq_) return 0;
  snd_seq_client_info_t* client = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_client_info_set_client(client, -1);
  int best = -1;
  uint8_t bestPorts = 0;
  char bestName[64] = {0};
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    // Hardware only: a kernel client backed by a card. Software sinks - a DAW, a synth, the
    // loopback - are not what "the interface" means, and guessing one of those would send
    // the sequence somewhere surprising.
    if (snd_seq_client_info_get_type(client) != SND_SEQ_KERNEL_CLIENT) continue;
    if (snd_seq_client_info_get_card(client) < 0) continue;
    const char* name = snd_seq_client_info_get_name(client);
    if (!name || isNotAnInterface(name)) continue;
    // The one with the most inputs wins, so a 4x4 interface is preferred over a keyboard that
    // happens to enumerate first - a controller has one input, for its own lights. With only a
    // controller plugged in it is still the answer, which is what "midiout" is for.
    const uint8_t ports = countSinkPorts(id);
    if (ports > bestPorts) {
      bestPorts = ports;
      best = id;
      std::strncpy(bestName, name, sizeof(bestName) - 1);
    }
  }
  return best >= 0 ? connectTo(best, bestName) : 0;
}

bool AlsaMidiOutput::portConnected(uint8_t port) const {
  if (!seq_ || port >= kNumMidiPorts || ports_[port] < 0) return false;
  snd_seq_query_subscribe_t* query = NULL;
  snd_seq_query_subscribe_alloca(&query);
  snd_seq_addr_t address;
  address.client = static_cast<unsigned char>(snd_seq_client_id(seq_));
  address.port = static_cast<unsigned char>(ports_[port]);
  snd_seq_query_subscribe_set_root(query, &address);
  snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_READ);
  snd_seq_query_subscribe_set_index(query, 0);
  return snd_seq_query_port_subscribers(seq_, query) >= 0;
}

bool AlsaMidiOutput::anyPortConnected() const {
  for (uint8_t port = 0; port < kNumMidiPorts; ++port) {
    if (portConnected(port)) return true;
  }
  return false;
}

void AlsaMidiOutput::send(const MidiMessage& message) {
  if (!seq_) return;
  if (message.port >= kNumMidiPorts || ports_[message.port] < 0) {
    ++dropped_;
    return;
  }
  const uint8_t length = messageLength(message.status);
  if (length == 0) return;

  // Built by hand rather than through snd_midi_event, which keeps running status between
  // messages: these go out on different ports and must each stand alone.
  snd_seq_event_t event;
  snd_seq_ev_clear(&event);
  const uint8_t channel = message.status & 0x0F;
  switch (message.status & 0xF0) {
    case kMidiNoteOn:
      snd_seq_ev_set_noteon(&event, channel, message.data1, message.data2);
      break;
    case kMidiNoteOff:
      snd_seq_ev_set_noteoff(&event, channel, message.data1, message.data2);
      break;
    case kMidiControlChange:
      snd_seq_ev_set_controller(&event, channel, message.data1, message.data2);
      break;
    case kMidiProgramChange:
      snd_seq_ev_set_pgmchange(&event, channel, message.data1);
      break;
    case 0xA0:
      snd_seq_ev_set_keypress(&event, channel, message.data1, message.data2);
      break;
    case 0xD0:
      snd_seq_ev_set_chanpress(&event, channel, message.data1);
      break;
    case 0xE0:
      snd_seq_ev_set_pitchbend(
          &event, channel,
          static_cast<int>((message.data2 << 7) | message.data1) - 8192);
      break;
    default:
      // Realtime: clock, start, continue and stop, once the sequencer sends them.
      switch (message.status) {
        case 0xF8: event.type = SND_SEQ_EVENT_CLOCK; break;
        case 0xFA: event.type = SND_SEQ_EVENT_START; break;
        case 0xFB: event.type = SND_SEQ_EVENT_CONTINUE; break;
        case 0xFC: event.type = SND_SEQ_EVENT_STOP; break;
        default: return;
      }
      break;
  }

  snd_seq_ev_set_source(&event, ports_[message.port]);
  snd_seq_ev_set_subs(&event);
  snd_seq_ev_set_direct(&event);
  for (int attempt = 0; attempt < kSendAttempts; ++attempt) {
    const int result = snd_seq_event_output_direct(seq_, &event);
    if (result >= 0) return;
    if (result != -EAGAIN) break;
    usleep(kSendRetryUs);
  }
  ++dropped_;
}

}  // namespace gx
