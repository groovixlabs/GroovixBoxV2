#pragma once

#include <stddef.h>
#include <stdint.h>

#include "engine/Project.h"

namespace gx {

// Binary project format, version 1: a little-endian byte layout with a magic tag, independent
// of compiler padding and CPU endianness, so projects move between devices.
//
// It is sparse. Only tracks, patterns and steps that differ from a new project are written,
// each level picked out by a bitmap, so an eight-step beat is a few hundred bytes instead of
// megabytes while the worst case stays bounded.
//
//   header:  magic(4) version(1) bpm(2) scale root(1) scale(1)
//            tracks(1) patterns(1) steps(2)        <- what the file holds, not this build
//            track bitmap(ceil(tracks/8))
//   track:   selected(1) preset(2) note(1) octave(1) layout(1) own root(1) own scale(1)
//            piano roll(1) channel(1) port(1) instrument(1) chord(1)
//            arp mode(1) arp rate(1) arp octaves(1)   <- version 8
//            pattern bitmap(ceil(patterns/8))
//   pattern: length(2) step bitmap(ceil(steps/8))
//   step:    active(1) count(1) notes(kMaxStepNotes) velocity(1) probability(1) gate(1)
//            nudge(1, version 5) ratchet(1, version 7)
//   scenes:  count(1) bitmap(ceil(count/8)) then 4 bytes of muted tracks per used scene
//   song:    count(1) bitmap(ceil(count/8)) then the scene each filled step plays
//
// Version 1 is the same file without the scene section, and still loads; so do 2, 3 and 4,
// whose headers are a byte shorter (no swing) and whose scenes hold no patterns, and 5, whose
// track headers are a byte shorter (no chord shape).
static const size_t kProjectHeaderSize = 14;      // 13 before swing was added (version 5)
static const size_t kProjectHeaderSizeV4 = 13;
static const size_t kEncodedTrackHeaderSize = 16;  // the arp's three came in version 8
static const size_t kEncodedTrackHeaderSizeV7 = 13;  // chords, version 6
static const size_t kEncodedTrackHeaderSizeV5 = 12;
static const size_t kEncodedStepSize = 7 + kMaxStepNotes;      // ratchet came in version 7
static const size_t kEncodedStepSizeV6 = 6 + kMaxStepNotes;    // micro-timing, version 5
static const size_t kEncodedStepSizeV4 = 5 + kMaxStepNotes;
static const size_t kTrackBitmapSize = (kNumTracks + 7) / 8;
static const size_t kPatternBitmapSize = (kNumPatterns + 7) / 8;
static const size_t kStepBitmapSize = (kMaxSteps + 7) / 8;
static const size_t kMaxEncodedPatternSize =
    2 + kStepBitmapSize + kMaxSteps * kEncodedStepSize;
static const size_t kMaxEncodedTrackSize =
    kEncodedTrackHeaderSize + kPatternBitmapSize + kNumPatterns * kMaxEncodedPatternSize;
// An upper bound: every track, pattern and step holding something. Real files are far smaller.
static const size_t kSceneBitmapSize = (kNumScenes + 7) / 8;
// Per used scene: 4 bytes of mutes, then a bitmap and a byte for each track's pattern.
static const size_t kMaxEncodedScenesSize =
    1 + kSceneBitmapSize + kNumScenes * (4 + kTrackBitmapSize + kNumTracks);
static const size_t kSongBitmapSize = (kNumSongSteps + 7) / 8;
static const size_t kMaxEncodedSongSize = 1 + kSongBitmapSize + kNumSongSteps;
static const size_t kMaxEncodedProjectSize = kProjectHeaderSize + kTrackBitmapSize +
                                             kNumTracks * kMaxEncodedTrackSize +
                                             kMaxEncodedScenesSize + kMaxEncodedSongSize;

// Writes a project. Returns the number of bytes written, which depends on what the project
// holds, or 0 if capacity is too small.
size_t encodeProject(const Project& project, uint8_t* out, size_t capacity);

// Reads a project, including files written by a build with fewer tracks, patterns or steps:
// whatever the file leaves out keeps its default. A file with more tracks than this build
// keeps its first kNumTracks; one claiming more patterns or steps than this build is refused
// rather than silently losing them. Returns false, leaving project untouched, if the data is
// not a valid project.
bool decodeProject(const uint8_t* data, size_t size, Project& project);

}  // namespace gx
