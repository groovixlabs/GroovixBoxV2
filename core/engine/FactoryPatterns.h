#pragma once

#include <stdint.h>

#include "engine/Project.h"

namespace gx {

// The built-in pattern bank: the upper half of every track's patterns - pages 5 to 8 of the
// eight, patterns 33 to 64 - holds material that ships with the instrument instead of
// material you made.
//
// It is read-only and it is not part of a project. The same patterns are there in every
// project, no project file carries them, and clearing a project does not disturb them. To
// make one yours, copy it down into pages 1 to 4 with Duplicate and edit the copy: that is
// the same gesture a pattern copy has always used, so there is nothing new to learn and no
// way to lose the original.
//
// What a track's bank holds follows the track's place in its page of eight, so the roles
// repeat: tracks 1, 9, 17 ... are drum tracks, tracks 2, 10, 18 ... are bass, and so on.
// Tracks 6 to 8 of each group have no bank - their pages 5 to 8 are empty and yours.
enum FactoryRole {
  kFactoryDrums = 0,
  kFactoryBass,
  kFactoryChords,
  kFactoryArp,
  kFactoryFills,
  kNumFactoryRoles,
};

// Tracks are grouped in eights, the same eight the surface shows as a track page. Kept here
// rather than taken from the UI, which the engine must not depend on; the static_assert in
// the .cpp ties it to the grouping the ports already use so the two cannot drift apart.
static const uint8_t kFactoryTrackGroup = 8;

// Patterns from here up are the bank. Half the patterns a build has, which on the desktop's
// 64 is patterns 33-64 - the bottom half of pattern mode's grid.
static const uint8_t kFirstFactoryPattern = kNumPatterns / 2;

// How many of each role's slots hold something. The rest of the bank is empty, left for the
// set to grow into without moving what is already there.
static const uint8_t kFactoryPatternsPerRole = 8;

// Whether this pattern index belongs to the bank rather than to the project.
inline bool isFactoryPattern(uint8_t pattern) {
  return pattern >= kFirstFactoryPattern && pattern < kNumPatterns;
}

// What this track's bank holds, or kNumFactoryRoles for a track that has no bank.
uint8_t factoryRole(uint8_t track);

// The bank's pattern for a track and slot, never null for an index the caller has already
// checked with isFactoryPattern: a slot the set does not reach yet reads as an empty pattern,
// so the pads show it dark and playing it is silence rather than a fault.
const Pattern* factoryPattern(uint8_t track, uint8_t pattern);

// What it is called, for the screen. Empty for a slot the set does not reach.
const char* factoryPatternName(uint8_t track, uint8_t pattern);

// Builds the bank. Called once before anything reads it; calling it again is harmless.
void initFactoryPatterns();

}  // namespace gx
