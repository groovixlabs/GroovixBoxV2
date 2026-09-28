#include "ui/Palette.h"

#include "ui/Controls.h"

namespace gx {

namespace {

const Rgb kTrackColors[kNumBottomButtons] = {
    {255, 189, 108},  // B1 peach: red is now the colour of a marker rather than a track -
                      // the last step, and recording - so track 1 needed another warm
                      // colour. It is one of the device's own 128, so it shows exactly, and
                      // it parts from B2's orange by being pale rather than by hue, which
                      // holds up better when both are dimmed than a neighbouring hue would
    {255, 130, 0},   // B2 orange
    {255, 225, 0},   // B3 yellow
    {50, 230, 60},   // B4 green
    {0, 215, 200},   // B5 cyan
    {40, 100, 255},  // B6 blue
    {160, 70, 255},  // B7 purple
    {255, 60, 170},  // B8 pink
};

}  // namespace

Rgb trackColor(uint8_t track) { return kTrackColors[track % kNumBottomButtons]; }

}  // namespace gx
