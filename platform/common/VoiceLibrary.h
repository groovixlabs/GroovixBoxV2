#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "comm/VoiceTable.h"
#include "engine/Project.h"
#include "ui/PresetCatalog.h"

namespace gx {

// The voice lists a rig has read, and which device each belongs to.
//
//   config/voices.conf
//     sx920.device = PSR-SX920               matched like midiout: part of the name, any case
//     sx920.file   = voices/psr-sx-920.csv   a CSV under the config directory
//
// The CSV is the maker's own list, pasted in whole. It says what its columns are in a header
// row, so nothing about its layout is configured: the reader finds MSB, LSB, PC and Name by
// name, and a file that doesn't carry all four is refused rather than read sideways.
//
// Preset slot n is the nth voice of the file, in the maker's order. A device nothing is said
// about plays VoiceTable's built-in General MIDI list.
//
// This is the desktop reader: it holds names as well as addresses, for the status line and
// the settings page. An MCU platform would keep the addresses alone and put them in flash -
// core only ever sees a VoiceTable, which is a pointer and a count either way.
class VoiceLibrary : public VoiceSource, public PresetCatalog {
 public:
  VoiceLibrary() {}
  ~VoiceLibrary() override;
  VoiceLibrary(const VoiceLibrary&) = delete;
  VoiceLibrary& operator=(const VoiceLibrary&) = delete;

  // Reads voices.conf from the config directory and every CSV it names, reporting what it
  // read and what it refused on `out`, one line each. A missing voices.conf is not an error
  // and leaves every device on the built-in list. Call once at startup.
  void load(const std::string& configDir, std::FILE* out);

  // Which list a device plays from, matched the way midiout and p1..p8 match: the configured
  // name is part of the device's, ignoring case. The built-in list when nothing matches.
  const VoiceTable& voicesForDevice(const std::string& deviceName) const;

  // What each port is wired to now, so a port re-patched by a REFRESH plays the new device's
  // voices with nothing rebound. An empty name puts the port back on the built-in list.
  void setPortDevice(uint8_t port, const std::string& deviceName);
  const std::string& portDevice(uint8_t port) const;

  // VoiceSource: the list for a port, by the device setPortDevice() last named.
  const VoiceTable& voicesFor(uint8_t port) const override;

  // PresetCatalog: how many voices that list has, so preset mode can darken the pads and
  // windows past its end.
  uint16_t presetCount(uint8_t port) const override;

  // The name of a slot on a port's device, for the status line and the log. Empty when the
  // list has no name for it, which is every slot of the built-in list.
  std::string voiceName(uint8_t port, uint16_t slot) const;

  // The slot and name of a voice on a port, found by the address itself. The log sees only
  // what goes on the wire, so this is how a Program Change gets named after the fact. False
  // when that port's list has no such voice.
  bool findVoice(uint8_t port, const VoiceAddress& address, uint16_t& slot,
                 std::string& name) const;

  // One line per port for the startup report and the devices page:
  //   "P1 Yamaha PSR-SX920 - 1650 from voices/psr-sx-920.csv (2 rows skipped)"
  std::string describePort(uint8_t port) const;

 private:
  struct List {
    std::string device;                 // what the config said to match
    std::string file;                   // where it came from, for reporting
    std::vector<VoiceAddress> entries;  // slot n is entries[n]
    std::vector<std::string> names;     // the same rows, named
    uint16_t skipped;                   // rows that were not voices
    VoiceTable table;                   // a view of entries, handed to core
    List() : skipped(0) {}
  };

  // Reads one CSV into `list`. False, with why on `out`, when the file cannot be read as a
  // voice list - a list loads whole or not at all, because a dropped row would shift every
  // slot after it and quietly change what saved projects play.
  bool readCsv(const std::string& path, List& list, std::FILE* out) const;

  const List* listForDevice(const std::string& deviceName) const;

  // Held by pointer so a list's entries never move: VoiceTable is a view into them.
  std::vector<List*> lists_;
  std::string portDevice_[kNumMidiPorts];
  VoiceTable builtIn_;
};

}  // namespace gx
