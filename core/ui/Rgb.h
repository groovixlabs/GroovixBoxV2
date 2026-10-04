#pragma once

#include <stdint.h>

namespace gx {

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// Scales a colour's brightness by level/255.
inline Rgb dim(Rgb c, uint8_t level) {
  Rgb out = {static_cast<uint8_t>(c.r * level / 255),
             static_cast<uint8_t>(c.g * level / 255),
             static_cast<uint8_t>(c.b * level / 255)};
  return out;
}

// Lifts a colour towards white by amount/255, for marking a pad without hiding what it was
// already showing - a Shift hint over a row of notes, say.
inline Rgb lift(Rgb c, uint8_t amount) {
  Rgb out = {static_cast<uint8_t>(c.r + (255 - c.r) * amount / 255),
             static_cast<uint8_t>(c.g + (255 - c.g) * amount / 255),
             static_cast<uint8_t>(c.b + (255 - c.b) * amount / 255)};
  return out;
}

inline bool isLit(Rgb c) { return (c.r | c.g | c.b) != 0; }

static const Rgb kBlack = {0, 0, 0};
static const Rgb kWhite = {255, 255, 255};

}  // namespace gx
