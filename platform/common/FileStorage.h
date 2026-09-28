#pragma once

#include <string>

#include "storage/Storage.h"

namespace gx {

// Storage as one file per key ("<directory>/<key>.gxb") using only C stdio, so the same
// code works on Linux, Windows, macOS and inside an iOS app sandbox.
class FileStorage : public Storage {
 public:
  // Creates the directory, and any parent it needs, if it isn't there yet.
  // Empty = the current directory.
  explicit FileStorage(const std::string& directory);
  // False if the directory doesn't exist and couldn't be made: nothing will save. Worth
  // telling the user at start-up rather than when they quit.
  bool ready() const { return ready_; }

  bool read(const char* key, uint8_t* buffer, size_t capacity, size_t& size) override;
  bool write(const char* key, const uint8_t* data, size_t size) override;
  bool exists(const char* key) override;
  bool remove(const char* key) override;
  // Moves the file into "<directory>/trash/<date>_<time>/" instead of deleting it, so a
  // mistaken Clear can be undone by hand. Falls back to deleting if it cannot be moved -
  // clearing still has to clear.
  bool discard(const char* key) override;
  // Where the last discard put the file, or empty if it had to delete it or there was
  // nothing there. Worth showing a user who has just cleared something.
  const std::string& lastDiscardPath() const { return lastDiscardPath_; }

 private:
  std::string pathFor(const char* key) const;
  // A free name in this moment's trash folder, or empty if the folder could not be made.
  std::string trashPathFor(const char* key);

  std::string directory_;
  std::string lastDiscardPath_;
  bool ready_;
};

}  // namespace gx
