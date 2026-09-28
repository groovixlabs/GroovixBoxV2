#include "linux/AlsaMidiInput.h"

#include <cstring>

namespace gx {

namespace {

const long kCodecBufferSize = 256;
const unsigned kSinkCaps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
const unsigned kSourceCaps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

// Clients that offer notes but are not something to play: the kernel's loopback, and the
// surfaces the sequencer drives itself.
bool isNotAController(const char* name) {
  static const char* const kSkip[] = {"Midi Through", "APC mini", "MIDI Mix"};
  for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
    if (std::strstr(name, kSkip[i])) return true;
  }
  return false;
}

}  // namespace

AlsaMidiInput::AlsaMidiInput()
    : seq_(NULL), port_(-1), decoder_(NULL), count_(0), at_(0) {}

AlsaMidiInput::~AlsaMidiInput() {
  if (decoder_) snd_midi_event_free(decoder_);
  if (seq_) snd_seq_close(seq_);
}

bool AlsaMidiInput::open(const char* clientName) {
  if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    seq_ = NULL;
    return false;
  }
  snd_seq_set_client_name(seq_, clientName);
  port_ = snd_seq_create_simple_port(seq_, "In", kSinkCaps,
                                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (port_ < 0 || snd_midi_event_new(kCodecBufferSize, &decoder_) < 0) return false;
  snd_midi_event_no_status(decoder_, 1);  // every decoded message carries its own status
  return true;
}

bool AlsaMidiInput::subscribe(int client, int port, const char* name) {
  if (snd_seq_connect_from(seq_, port_, client, port) < 0) return false;
  deviceName_ = name ? name : "";
  return true;
}

bool AlsaMidiInput::connect(const char* clientNameContains) {
  if (!seq_ || !clientNameContains) return false;
  snd_seq_client_info_t* client = NULL;
  snd_seq_port_info_t* info = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_alloca(&info);
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    const char* name = snd_seq_client_info_get_name(client);
    if (!name || !std::strstr(name, clientNameContains)) continue;
    snd_seq_port_info_set_client(info, id);
    snd_seq_port_info_set_port(info, -1);
    while (snd_seq_query_next_port(seq_, info) >= 0) {
      if ((snd_seq_port_info_get_capability(info) & kSourceCaps) != kSourceCaps) continue;
      if (subscribe(id, snd_seq_port_info_get_port(info), name)) return true;
    }
  }
  return false;
}

bool AlsaMidiInput::connectFirstController() {
  if (!seq_) return false;
  snd_seq_client_info_t* client = NULL;
  snd_seq_port_info_t* info = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_alloca(&info);
  snd_seq_client_info_set_client(client, -1);
  int best = -1, bestPort = -1;
  unsigned bestInputs = 0;
  char bestName[64] = {0};
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    if (snd_seq_client_info_get_type(client) != SND_SEQ_KERNEL_CLIENT) continue;
    if (snd_seq_client_info_get_card(client) < 0) continue;
    const char* name = snd_seq_client_info_get_name(client);
    if (!name || isNotAController(name)) continue;

    int source = -1;
    unsigned inputs = 0;
    snd_seq_port_info_set_client(info, id);
    snd_seq_port_info_set_port(info, -1);
    while (snd_seq_query_next_port(seq_, info) >= 0) {
      const unsigned caps = snd_seq_port_info_get_capability(info);
      if ((caps & kSinkCaps) == kSinkCaps) ++inputs;
      if (source < 0 && (caps & kSourceCaps) == kSourceCaps) {
        source = snd_seq_port_info_get_port(info);
      }
    }
    if (source < 0) continue;  // nothing to listen to
    // The fewest inputs wins: an interface has one per socket, a keyboard has the one it
    // lights its pads through, so this is the opposite of how the output picks its device.
    if (best < 0 || inputs < bestInputs) {
      best = id;
      bestPort = source;
      bestInputs = inputs;
      std::strncpy(bestName, name, sizeof(bestName) - 1);
    }
  }
  return best >= 0 && subscribe(best, bestPort, bestName);
}

bool AlsaMidiInput::connected() const {
  if (!seq_ || port_ < 0) return false;
  snd_seq_query_subscribe_t* query = NULL;
  snd_seq_query_subscribe_alloca(&query);
  snd_seq_addr_t address;
  address.client = static_cast<unsigned char>(snd_seq_client_id(seq_));
  address.port = static_cast<unsigned char>(port_);
  snd_seq_query_subscribe_set_root(query, &address);
  snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
  snd_seq_query_subscribe_set_index(query, 0);
  return snd_seq_query_port_subscribers(seq_, query) >= 0;
}

bool AlsaMidiInput::poll(MidiMessage& message) {
  if (!seq_) return false;
  for (;;) {
    while (at_ < count_) {
      const uint8_t byte = bytes_[at_++];
      if (parser_.feed(byte, message)) return true;
    }
    at_ = 0;
    count_ = 0;
    snd_seq_event_t* event = NULL;
    if (snd_seq_event_input(seq_, &event) < 0 || !event) return false;
    const long written = snd_midi_event_decode(decoder_, bytes_, sizeof(bytes_), event);
    if (written > 0) count_ = static_cast<uint8_t>(written);
    snd_midi_event_reset_decode(decoder_);
  }
}

}  // namespace gx
