#include "linux/AlsaAnnounce.h"

namespace gx {

AlsaAnnounce::AlsaAnnounce() : seq_(NULL), port_(-1) {}

AlsaAnnounce::~AlsaAnnounce() {
  if (seq_) snd_seq_close(seq_);
}

bool AlsaAnnounce::open(const char* clientName) {
  if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    seq_ = NULL;
    return false;
  }
  snd_seq_set_client_name(seq_, clientName);
  // We only ever receive here, and the announcements are the only thing that writes to us.
  port_ = snd_seq_create_simple_port(seq_, "Announce",
                                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                     SND_SEQ_PORT_TYPE_APPLICATION);
  if (port_ < 0 ||
      snd_seq_connect_from(seq_, port_, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE) < 0) {
    snd_seq_close(seq_);
    seq_ = NULL;
    port_ = -1;
    return false;
  }
  return true;
}

bool AlsaAnnounce::takeChange(std::vector<int>& arrived) {
  arrived.clear();
  if (!seq_) return false;
  bool changed = false;
  snd_seq_event_t* event = NULL;
  // Drain the lot: several events arrive for one device, and one answer covers them all.
  while (snd_seq_event_input(seq_, &event) >= 0 && event) {
    switch (event->type) {
      case SND_SEQ_EVENT_CLIENT_START:
      case SND_SEQ_EVENT_PORT_START: {
        const int client = event->data.addr.client;
        bool known = false;
        for (size_t i = 0; i < arrived.size(); ++i) {
          if (arrived[i] == client) known = true;
        }
        if (!known) arrived.push_back(client);  // a device announces each of its ports
        changed = true;
        break;
      }
      case SND_SEQ_EVENT_CLIENT_EXIT:
      case SND_SEQ_EVENT_PORT_EXIT:
        changed = true;
        break;
      default:
        break;  // subscriptions and the rest: not gear coming or going
    }
  }
  return changed;
}

}  // namespace gx
