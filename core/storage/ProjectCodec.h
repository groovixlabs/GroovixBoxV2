#pragma once

#include <stddef.h>
#include <stdint.h>

#include "engine/Project.h"

namespace gx {

// Binary project format, version 0: a little-endian byte layout with a magic tag, independent
// of compiler padding and CPU endianness, so projects move between devices.
//
// It is sparse. Only tracks, patterns and steps that differ from a new project are written,
// each level picked out by a bitmap, so an eight-step beat is a few hundred bytes instead of
// megabytes while the worst case stays bounded.
//
//   header:  magic(4) version(1) bpm(2) scale root(1) scale(1) swing(1)
//            tracks(1) patterns(1) steps(2)        <- what the file holds, not this build
//            track bitmap(ceil(tracks/8))
//   track:   selected(1) preset(2) note(1) octave(1) layout(1) own root(1) own scale(1)
//            piano roll(1) channel(1) port(1) instrument(1) chord(1)
//            arp mode(1) arp rate(1) arp octaves(1)
//            pattern bitmap(ceil(patterns/8))
//   pattern: length(2) step bitmap(ceil(steps/8))
//   step:    active(1) count(1) notes(kMaxStepNotes) velocity(1) probability(1) gate(1)
//            nudge(1) ratchet(1)
//   scenes:  count(1) bitmap(ceil(count/8)), then per used scene a muted-track bitmap
//            (ceil(tracks/8)), a pattern bitmap (ceil(tracks/8)) and a byte per named pattern
//   song:    count(1) bitmap(ceil(count/8)) then the scene each filled step plays
//
// One version, one layout. Version 0 is the first format of the shipping file and nothing
// before it loads: the versions that grew the header a field at a time are gone, along with
// the code that read them. A file whose version byte is not 0 is refused.
//
// What does carry across builds is capacity. The header says how many tracks, patterns and
// steps the file holds, so a project written by a small build opens on a large one and
// everything it leaves out keeps its default. That is not legacy support - it is how the MCU
// build and the desktop build share files.
static const size_t kProjectHeaderSize = 14;
static const size_t kEncodedTrackHeaderSize = 16;
static const size_t kEncodedStepSize = 7 + kMaxStepNotes;
static const size_t kTrackBitmapSize = (kNumTracks + 7) / 8;
static const size_t kPatternBitmapSize = (kNumPatterns + 7) / 8;
static const size_t kStepBitmapSize = (kMaxSteps + 7) / 8;
static const size_t kMaxEncodedPatternSize =
    2 + kStepBitmapSize + kMaxSteps * kEncodedStepSize;
static const size_t kMaxEncodedTrackSize =
    kEncodedTrackHeaderSize + kPatternBitmapSize + kNumPatterns * kMaxEncodedPatternSize;
// An upper bound: every track, pattern and step holding something. Real files are far smaller.
static const size_t kSceneBitmapSize = (kNumScenes + 7) / 8;
// Per used scene: a bitmap of muted tracks, then a bitmap and a byte for each track's pattern.
static const size_t kMaxEncodedScenesSize =
    1 + kSceneBitmapSize + kNumScenes * (2 * kTrackBitmapSize + kNumTracks);
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
