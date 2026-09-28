#include "app/GroovixApp.h"

namespace gx {

static_assert(kNumProjectSlots <= SlotStore::kMaxSlots && kNumPresetSlots <= SlotStore::kMaxSlots,
              "slot counts exceed the SlotStore index");

namespace {

// Remembers which project was open: {2, slot low byte, slot high byte}.
// Version 1, from before pages, was {1, slot}.
const char* const kSessionKey = "session";
const uint8_t kSessionVersion = 2;

// How long Auto waits before deciding the incoming clock has stopped. Four clocks even at
// kMinBpm, so it never gives up on a tempo we can play.
const uint32_t kClockTimeoutUs = 500000;

}  // namespace

GroovixApp::GroovixApp(ControlSurface& surface, Storage& storage, EventSink& output)
    : surface_(surface),
      storage_(storage),
      extras_(NULL),
      midiIn_(NULL),
      lastClockUs_(0),
      clockSeen_(false),
      projects_(storage, "project", kNumProjectSlots),
      sequencer_(output),
      ui_(sequencer_, *this),
      currentProject_(0) {
  frame_.clear();
}

void GroovixApp::begin() {
  projects_.scan();

  uint8_t session[3];
  size_t size = 0;
  uint16_t slot = 0;
  if (storage_.read(kSessionKey, session, sizeof(session), size)) {
    if (size == 3 && session[0] == kSessionVersion) {
      slot = static_cast<uint16_t>(session[1] | (session[2] << 8));
    } else if (size == 2 && session[0] == 1) {
      slot = session[1];
    }
  }
  openProject(slot < kNumProjectSlots ? slot : 0);
}

void GroovixApp::update(uint32_t nowMs) {
  handleInput(nowMs);
  handleMidiInput(nowMs * 1000u);
  sequencer_.update(nowMs);
  refreshLeds(nowMs);
}

// On a threaded build this runs on the clock thread, every millisecond. What comes in over
// MIDI is read here rather than with the surface, because an incoming clock is a fifth of a
// step apart at 120 BPM and reading it at the frame rate would smear the beat.
void GroovixApp::tick(uint32_t nowUs) {
  handleMidiInput(nowUs);
  sequencer_.updateMicros(nowUs);
}

void GroovixApp::updateSurface(uint32_t nowMs) {
  handleInput(nowMs);
  refreshLeds(nowMs);
}

bool GroovixApp::saveProject() {
  if (isProjectEmpty(sequencer_.project())) {
    if (extras_) extras_->clearProject(currentProject_);
    return projects_.remove(currentProject_);
  }
  const size_t size = encodeProject(sequencer_.project(), projectBuffer_, sizeof(projectBuffer_));
  if (size == 0 || !projects_.write(currentProject_, projectBuffer_, size)) return false;
  if (extras_) extras_->saveProject(currentProject_);
  return true;
}

// ---- Library ----

bool GroovixApp::projectHasData(uint16_t slot) const { return projects_.hasData(slot); }

bool GroovixApp::projectPageHasData(uint8_t page) const {
  return projects_.anyData(static_cast<uint16_t>(page * kSlotsPerPage), kSlotsPerPage);
}

void GroovixApp::selectProject(uint16_t slot) {
  if (slot >= kNumProjectSlots || slot == currentProject_) return;
  saveProject();
  openProject(slot);
}

void GroovixApp::clearProject(uint16_t slot) {
  projects_.remove(slot);
  if (extras_) extras_->clearProject(slot);
  if (slot == currentProject_) resetProject();
}

void GroovixApp::copyProject(uint16_t from, uint16_t to) {
  // Copy the open project as it is now, including unsaved edits.
  if (from == currentProject_) saveProject();
  if (projects_.copy(from, to, projectBuffer_, sizeof(projectBuffer_)) && to == currentProject_) {
    if (!loadProjectSlot(to)) resetProject();
  }
}

// ---- Private ----

void GroovixApp::handleInput(uint32_t nowMs) {
  ControlEvent event;
  ui_.setTime(nowMs);
  while (surface_.pollEvent(event)) ui_.handleEvent(event);
}

// Auto follows only while a clock is really arriving, so a rig that stops sending - or was
// never sending - plays on our own clock instead of looking broken. Half a second is four
// clocks even at the slowest tempo we allow.
void GroovixApp::updateClockFollowing(uint32_t nowUs) {
  const uint8_t source = sequencer_.clockSource();
  bool follow = source == kClockExternal;
  if (source == kClockAuto && clockSeen_ && nowUs - lastClockUs_ < kClockTimeoutUs) follow = true;
  sequencer_.setFollowingExternal(follow);
}

// What arrives from a keyboard. Notes reach the selected track and are recorded like a pad
// press; the transport keys drive playback, so a player's hands need not leave the keys.
// Anything else - aftertouch, bend, a controller's own CC - is let past for now.
void GroovixApp::handleMidiInput(uint32_t nowUs) {
  if (!midiIn_) {
    updateClockFollowing(nowUs);
    return;
  }
  MidiMessage message;
  while (midiIn_->poll(message)) {
    const uint8_t type = static_cast<uint8_t>(message.status & 0xF0);
    if (type == kMidiNoteOn) {
      // A note on with no velocity is how many keyboards say note off.
      ui_.playExternalNote(message.data1, message.data2, message.data2 > 0);
    } else if (type == kMidiNoteOff) {
      ui_.playExternalNote(message.data1, 0, false);
    } else if (message.status == kMidiClock) {
      // The stamp and the decision come first: the very first clock of a run is what puts
      // Auto into following, and it has to drive this tick rather than be thrown away.
      lastClockUs_ = nowUs;
      clockSeen_ = true;
      updateClockFollowing(nowUs);
      sequencer_.externalClockTick(nowUs);
    } else if (message.status == kMidiStart || message.status == kMidiContinue) {
      // Continue restarts, like Start: there is no song position to resume from yet.
      sequencer_.play();
    } else if (message.status == kMidiStop) {
      sequencer_.stop();
    }
  }
  updateClockFollowing(nowUs);
}

void GroovixApp::refreshLeds(uint32_t nowMs) {
  frame_.clear();
  ui_.render(frame_, nowMs);
  surface_.show(frame_);
}

void GroovixApp::openProject(uint16_t slot) {
  currentProject_ = slot;
  if (!loadProjectSlot(slot)) resetProject();
  if (extras_) extras_->openProject(slot);
  const uint8_t session[3] = {kSessionVersion, static_cast<uint8_t>(slot & 0xFF),
                              static_cast<uint8_t>(slot >> 8)};
  storage_.write(kSessionKey, session, sizeof(session));
}

bool GroovixApp::loadProjectSlot(uint16_t slot) {
  size_t size = 0;
  if (!projects_.read(slot, projectBuffer_, sizeof(projectBuffer_), size) ||
      !decodeProject(projectBuffer_, size, scratchProject_)) {
    return false;
  }
  sequencer_.setProject(scratchProject_);
  return true;
}

void GroovixApp::resetProject() {
  initProject(scratchProject_);
  sequencer_.setProject(scratchProject_);
}

}  // namespace gx
