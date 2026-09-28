#include "common/RigPaths.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>

// The built rig's stick. Overridable at build time for a rig that mounts its storage somewhere
// else, so a package doesn't have to patch this file: -DGX_DEFAULT_DATA_DIR=\"/var/lib/...\".
#ifndef GX_DEFAULT_DATA_DIR
#define GX_DEFAULT_DATA_DIR "/mnt/usb1/data"
#endif
#ifndef GX_DEFAULT_CONFIG_DIR
#define GX_DEFAULT_CONFIG_DIR "/mnt/usb1/config"
#endif

namespace gx {

const char* const kDefaultDataDir = GX_DEFAULT_DATA_DIR;
const char* const kDefaultConfigDir = GX_DEFAULT_CONFIG_DIR;

namespace {

bool isDirectory(const std::string& path) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) return false;
  return (info.st_mode & S_IFMT) == S_IFDIR;
}

std::string withSlash(const std::string& path) {
  if (path.empty()) return path;
  const char last = path[path.size() - 1];
  if (last == '/' || last == '\\') return path;  // a Windows path keeps its own separator
  return path + "/";
}

// "/mnt/usb1/data" -> "/mnt/usb1". Empty when there is no parent to speak of.
std::string parentOf(const std::string& path) {
  std::string::size_type end = path.size();
  while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
  while (end > 0 && path[end - 1] != '/' && path[end - 1] != '\\') --end;
  while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) --end;
  return path.substr(0, end);
}

// Where everything lived before the rig had a stick, and where a desktop still keeps it.
std::string userDir() {
  const char* dataHome = std::getenv("XDG_DATA_HOME");
  if (dataHome && *dataHome) return std::string(dataHome) + "/Groovix/GroovixBox/";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.local/share/Groovix/GroovixBox/";
}

// The default, unless the stick it lives on isn't there. The directory itself doesn't have to
// exist - FileStorage makes it - but the mount point above it does, and on a desktop it won't.
std::string onStick(const char* def, const char* what) {
  const std::string parent = parentOf(def);
  if (isDirectory(def) || (!parent.empty() && isDirectory(parent))) return withSlash(def);
  const std::string fallback = userDir();
  std::fprintf(stderr, "%s: '%s' is not mounted, so '%s' cannot be used — using '%s'\n", what,
               parent.c_str(), def, fallback.c_str());
  return fallback;
}

std::string resolve(const char* named, const char* variable, const char* def, const char* what) {
  if (named && *named) return withSlash(named);
  const char* fromEnvironment = std::getenv(variable);
  if (fromEnvironment && *fromEnvironment) return withSlash(fromEnvironment);
  return onStick(def, what);
}

}  // namespace

std::string rigDataDir(const char* named) {
  return resolve(named, "GXBOX_DATA_DIR", kDefaultDataDir, "data");
}

std::string rigConfigDir(const char* named) {
  return resolve(named, "GXBOX_CONFIG_DIR", kDefaultConfigDir, "config");
}

std::string rigConfigPath(const char* named, const std::string& configDir, const char* name) {
  if (named && *named) return named;
  return withSlash(configDir) + name;
}

}  // namespace gx
