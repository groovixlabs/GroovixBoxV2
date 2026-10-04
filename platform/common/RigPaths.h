// Where the rig keeps its files. One place, so the simulator and the headless build cannot
// drift apart and answer the same question differently.
//
// A built GroovixBox is a Pi with a USB stick in it, and the stick is the whole of its
// storage: /mnt/usb1/data for projects and presets, /mnt/usb1/config for controls.conf and
// instruments.conf. Both can be moved with a switch or with the environment, which is how the
// settings page is pointed at the same pair.

#ifndef GX_COMMON_RIGPATHS_H
#define GX_COMMON_RIGPATHS_H

#include <string>

namespace gx {

extern const char* const kDefaultDataDir;    // /mnt/usb1/data
extern const char* const kDefaultConfigDir;  // /mnt/usb1/config

// The rig's two directories, each returned with a trailing slash. A directory named on the
// command line wins; then $GXBOX_DATA_DIR / $GXBOX_CONFIG_DIR; then the default above.
//
// On a machine where the stick isn't mounted - a desktop running the simulator - each default
// falls back to the matching per-user directory, $XDG_DATA_HOME/Groovix/{data,config} or
// ~/.local/share/Groovix/{data,config}, and says on stderr that it did. The pair stays split
// there as it is on the stick. Falling back silently is what made the stick look ignored in
// the first place.
std::string rigDataDir(const char* named);
std::string rigConfigDir(const char* named);

// A config file inside the config directory, unless it was named outright.
std::string rigConfigPath(const char* named, const std::string& configDir, const char* name);

}  // namespace gx

#endif  // GX_COMMON_RIGPATHS_H
