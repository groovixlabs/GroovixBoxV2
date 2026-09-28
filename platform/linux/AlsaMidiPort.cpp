#include "linux/AlsaMidiPort.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace gx {

namespace {

const long kCodecBufferSize = 256;
const int kSendAttempts = 50;         // with 1 ms between them
const useconds_t kSendRetryUs = 1000;
const unsigned kDuplexCaps = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
                             SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;

}  // namespace

AlsaMidiPort::AlsaMidiPort() : seq_(NULL), port_(-1), decoder_(NULL), encoder_(NULL) {}

AlsaMidiPort::~AlsaMidiPort() { close(); }

void AlsaMidiPort::close() {
  if (decoder_) snd_midi_event_free(decoder_);
  if (encoder_) snd_midi_event_free(encoder_);
  if (seq_) snd_seq_close(seq_);
  decoder_ = NULL;
  encoder_ = NULL;
  seq_ = NULL;
  port_ = -1;
  received_.clear();
  portName_.clear();
}

bool AlsaMidiPort::open(const char* clientName, const char* portNameContains) {
  close();  // opening again after a device went away starts from nothing
  if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
    seq_ = NULL;
    return false;
  }
  snd_seq_set_client_name(seq_, clientName);
  port_ = snd_seq_create_simple_port(seq_, "Surface", kDuplexCaps,
                                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (port_ < 0 || snd_midi_event_new(kCodecBufferSize, &decoder_) < 0 ||
      snd_midi_event_new(kCodecBufferSize, &encoder_) < 0) {
    close();
    return false;
  }
  snd_midi_event_no_status(decoder_, 1);  // every decoded message starts with its status

  snd_seq_client_info_t* client = NULL;
  snd_seq_port_info_t* info = NULL;
  snd_seq_client_info_alloca(&client);
  snd_seq_port_info_alloca(&info);
  snd_seq_client_info_set_client(client, -1);
  while (snd_seq_query_next_client(seq_, client) >= 0) {
    const int id = snd_seq_client_info_get_client(client);
    if (id == snd_seq_client_id(seq_)) continue;
    snd_seq_port_info_set_client(info, id);
    snd_seq_port_info_set_port(info, -1);
    while (snd_seq_query_next_port(seq_, info) >= 0) {
      const char* name = snd_seq_port_info_get_name(info);
      if (!name || !std::strstr(name, portNameContains)) continue;
      if ((snd_seq_port_info_get_capability(info) & kDuplexCaps) != kDuplexCaps) continue;
      const int devicePort = snd_seq_port_info_get_port(info);
      if (snd_seq_connect_from(seq_, port_, id, devicePort) < 0 ||
          snd_seq_connect_to(seq_, port_, id, devicePort) < 0) {
        close();
        return false;
      }
      portName_ = name;
      return true;
    }
  }
  // The device isn't there. Let the client go rather than leaving one behind: a client that
  // exists is a client other programs list, and its own arrival is an event anything watching
  // the sequencer will see - including us, which would read as gear turning up.
  close();
  return false;
}

bool AlsaMidiPort::connected() const {
  if (!seq_ || port_ < 0) return false;
  snd_seq_query_subscribe_t* query = NULL;
  snd_seq_query_subscribe_alloca(&query);
  snd_seq_addr_t address;
  address.client = static_cast<unsigned char>(snd_seq_client_id(seq_));
  address.port = static_cast<unsigned char>(port_);
  snd_seq_query_subscribe_set_root(query, &address);
  snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_READ);
  snd_seq_query_subscribe_set_index(query, 0);
  return snd_seq_query_port_subscribers(seq_, query) >= 0;
}

size_t AlsaMidiPort::read(uint8_t* buffer, size_t capacity) {
  if (!seq_) return 0;
  while (received_.empty()) {
    snd_seq_event_t* event = NULL;
    if (snd_seq_event_input(seq_, &event) < 0 || !event) break;  // -EAGAIN: nothing waiting
    if (event->type == SND_SEQ_EVENT_SYSEX) {
      const uint8_t* bytes = static_cast<const uint8_t*>(event->data.ext.ptr);
      received_.insert(received_.end(), bytes, bytes + event->data.ext.len);
    } else {
      uint8_t bytes[16];
      const long count = snd_midi_event_decode(decoder_, bytes, sizeof(bytes), event);
      if (count > 0) received_.insert(received_.end(), bytes, bytes + count);
    }
  }
  const size_t count = std::min(capacity, received_.size());
  std::copy(received_.begin(), received_.begin() + count, buffer);
  received_.erase(received_.begin(), received_.begin() + count);
  return count;
}

bool AlsaMidiPort::write(const uint8_t* data, size_t size) {
  if (!seq_) return false;
  bool ok = true;
  snd_seq_event_t event;
  size_t i = 0;
  while (i < size) {
    if (data[i] == 0xF0) {
      // SysEx goes out whole, as one event.
      size_t end = i;
      while (end < size && data[end] != 0xF7) ++end;
      if (end == size) return false;
      snd_seq_ev_clear(&event);
      snd_seq_ev_set_sysex(&event, end - i + 1,
                           const_cast<void*>(static_cast<const void*>(data + i)));
      ok = send(event) && ok;
      i = end + 1;
      continue;
    }
    snd_seq_ev_clear(&event);
    long used = 0;
    while (i < size && data[i] != 0xF0) {
      used = snd_midi_event_encode_byte(encoder_, data[i++], &event);
      if (used == 1) break;  // a whole message
    }
    if (used == 1) ok = send(event) && ok;
  }
  return ok;
}

bool AlsaMidiPort::send(snd_seq_event_t& event) {
  snd_seq_ev_set_source(&event, port_);
  snd_seq_ev_set_subs(&event);
  snd_seq_ev_set_direct(&event);
  // The port is non-blocking, so a full output queue says "try again": wait briefly rather
  // than drop the message.
  for (int attempt = 0; attempt < kSendAttempts; ++attempt) {
    const int result = snd_seq_event_output_direct(seq_, &event);
    if (result >= 0) return true;
    if (result != -EAGAIN) return false;
    usleep(kSendRetryUs);
  }
  return false;
}

}  // namespace gx
