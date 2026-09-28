#pragma once

#include <stdint.h>

namespace gx {

// Anything a platform keeps alongside a project that the core knows nothing about — the state
// of hosted plugins, for one. The app says when a project is saved, opened or cleared; where
// the data goes is the platform's business, usually the same Storage under its own key.
class ProjectExtras {
 public:
  virtual void saveProject(uint16_t slot) = 0;
  virtual void openProject(uint16_t slot) = 0;
  virtual void clearProject(uint16_t slot) = 0;

 protected:
  ~ProjectExtras() {}  // see EventSink
};

}  // namespace gx
