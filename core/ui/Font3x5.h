#pragma once

#include <stdint.h>

namespace gx {

// Tiny 3x5 bitmap font covering ASCII ' ' to 'Z': values drawn on the pads, and the
// simulator's labels.
// Each glyph is five octal digits, one per row from the top; each digit's
// three bits are the row's pixels, left to right.
static const char kFontFirstChar = ' ';
static const char kFontLastChar = 'Z';
static const int kGlyphWidth = 3;
static const int kGlyphHeight = 5;

static const uint16_t kFont3x5[] = {
    000000, 022202, 055000, 057575, 036236, 051245, 025253, 022000,  //  !"#$%&'
    012221, 042224, 005250, 002720, 000024, 000700, 000002, 011244,  // ()*+,-./
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111,  // 01234567
    075757, 075717, 002020, 002024, 012421, 007070, 042124, 071202,  // 89:;<=>?
    075747, 025755, 065656, 034443, 065556, 074647, 074644, 034553,  // @ABCDEFG
    055755, 072227, 011152, 055655, 044447, 057755, 065555, 025552,  // HIJKLMNO
    065644, 025563, 065655, 034216, 072222, 055557, 055552, 055775,  // PQRSTUVW
    055255, 055222, 071247,                                          // XYZ
};

}  // namespace gx
