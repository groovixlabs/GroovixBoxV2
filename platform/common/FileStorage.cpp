#include "common/FileStorage.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#ifdef _WIN32
#include <direct.h>
#endif

namespace gx {

namespace {

// The trash keeps one folder per moment, so two clears in the same second share one; a name
// already taken in it gets a counter rather than overwriting the earlier file.
const int kMaxSameSecond = 99;

bool isFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return false;
  std::fclose(file);
  return true;
}

bool isDirectory(const std::string& path) {
  struct stat info;
  return stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFDIR) != 0;
}

bool makeDirectory(const std::string& path) {
#ifdef _WIN32
  const int result = _mkdir(path.c_str());
#else
  const int result = mkdir(path.c_str(), 0755);
#endif
  return result == 0 || errno == EEXIST;
}

// Creates every level of a path, so --data ~/new/place works the first time.
bool makeDirectories(const std::string& path) {
  for (size_t at = path.find_first_of("/\\"); at != std::string::npos;
       at = path.find_first_of("/\\", at + 1)) {
    if (at == 0) continue;  // the leading slash of an absolute path
    if (!makeDirectory(path.substr(0, at))) return false;
  }
  return isDirectory(path);
}

}  // namespace

FileStorage::FileStorage(const std::string& directory) : directory_(directory), ready_(true) {
  if (directory_.empty()) return;
  const char last = directory_[directory_.size() - 1];
  if (last != '/' && last != '\\') directory_ += '/';
  ready_ = isDirectory(directory_) || makeDirectories(directory_);
}

std::string FileStorage::pathFor(const char* key) const {
  return directory_ + key + ".gxb";
}

bool FileStorage::read(const char* key, uint8_t* buffer, size_t capacity, size_t& size) {
  std::FILE* file = std::fopen(pathFor(key).c_str(), "rb");
  if (!file) return false;
  size = std::fread(buffer, 1, capacity, file);
  // Reaching end-of-file right after the read means the whole value fitted.
  const bool ok = !std::ferror(file) && std::fgetc(file) == EOF;
  std::fclose(file);
  return ok;
}

bool FileStorage::write(const char* key, const uint8_t* data, size_t size) {
  // Write a temporary file first so a failed write never destroys the previous value.
  const std::string path = pathFor(key);
  const std::string tempPath = path + ".tmp";

  std::FILE* file = std::fopen(tempPath.c_str(), "wb");
  if (!file) return false;
  const bool written = std::fwrite(data, 1, size, file) == size;
  if (std::fclose(file) != 0 || !written) {
    std::remove(tempPath.c_str());
    return false;
  }

  if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
    // Windows will not rename over an existing file.
    std::remove(path.c_str());
    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
      std::remove(tempPath.c_str());
      return false;
    }
  }
  return true;
}

bool FileStorage::exists(const char* key) {
  std::FILE* file = std::fopen(pathFor(key).c_str(), "rb");
  if (!file) return false;
  std::fclose(file);
  return true;
}

bool FileStorage::remove(const char* key) {
  return std::remove(pathFor(key).c_str()) == 0 || !exists(key);
}

std::string FileStorage::trashPathFor(const char* key) {
  const std::time_t now = std::time(0);
  const std::tm* local = std::localtime(&now);
  char stamp[32];
  if (!local || std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", local) == 0) {
    // No usable clock: the folder still has to be unique and sortable.
    std::snprintf(stamp, sizeof(stamp), "%lu", static_cast<unsigned long>(now));
  }
  const std::string folder = directory_ + "trash/" + stamp + "/";
  if (!makeDirectories(folder)) return std::string();

  std::string target = folder + key + ".gxb";
  for (int n = 2; n <= kMaxSameSecond && isFile(target); ++n) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%d", n);
    target = folder + key + suffix + ".gxb";
  }
  return isFile(target) ? std::string() : target;
}

bool FileStorage::discard(const char* key) {
  lastDiscardPath_.clear();
  if (!exists(key)) return true;  // nothing to keep, and no empty folder left behind
  const std::string from = pathFor(key);
  const std::string to = trashPathFor(key);
  if (!to.empty() && std::rename(from.c_str(), to.c_str()) == 0) {
    lastDiscardPath_ = to;
    return true;
  }
  return remove(key);
}

}  // namespace gx
