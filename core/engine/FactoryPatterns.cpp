#include "engine/FactoryPatterns.h"

namespace gx {

namespace {

// The bank is written as note lists rather than as whole patterns: a Pattern is mostly empty
// steps, and spelling 256 of them out forty times over would be unreadable and enormous. The
// lists are expanded once into real patterns at start-up, after which every reader gets an
// ordinary `const Pattern*` and nothing else in the engine needs to know the bank exists.
struct FactoryNote {
  uint8_t step;
  uint8_t note;
  uint8_t velocity;
};

struct FactoryEntry {
  const char* name;
  uint8_t length;
  const FactoryNote* notes;
  uint8_t count;
};

// General MIDI drum map, which is what a drum track's keyboard lays out.
const uint8_t kKick = 36, kRim = 37, kSnare = 38, kClap = 39;
const uint8_t kClosedHat = 42, kLowTom = 45, kOpenHat = 46, kMidTom = 47;
const uint8_t kCrash = 49, kHighTom = 50;

// Velocities: an accent, the ordinary hit, and a ghost note under the beat.
const uint8_t kAccent = 120, kHit = 100, kGhost = 60;

// ---- drums, one bar of sixteenths ----

const FactoryNote kFourFloor[] = {
    {0, kKick, kAccent},   {2, kClosedHat, 80},  {4, kKick, kHit},     {4, kClap, 110},
    {6, kClosedHat, 80},   {8, kKick, kAccent},  {10, kClosedHat, 80}, {12, kKick, kHit},
    {12, kClap, 110},      {14, kOpenHat, 90}};
const FactoryNote kBackbeat[] = {
    {0, kKick, kAccent},  {0, kClosedHat, 90},  {2, kClosedHat, 70}, {4, kSnare, 115},
    {4, kClosedHat, 90},  {6, kClosedHat, 70},  {8, kKick, 110},     {8, kClosedHat, 90},
    {10, kClosedHat, 70}, {12, kSnare, 115},    {12, kClosedHat, 90}, {14, kClosedHat, 70}};
const FactoryNote kBreakbeat[] = {
    {0, kKick, kAccent}, {0, kClosedHat, 85},  {2, kClosedHat, 70}, {4, kSnare, 115},
    {4, kClosedHat, 85}, {6, kKick, kHit},     {6, kClosedHat, 70}, {7, kSnare, 55},
    {8, kClosedHat, 85}, {10, kKick, 105},     {10, kClosedHat, 70}, {12, kSnare, 115},
    {12, kClosedHat, 85}, {14, kClosedHat, 70}};
const FactoryNote kHalfTime[] = {{0, kKick, kAccent},  {0, kClosedHat, 90}, {4, kClosedHat, 80},
                                 {8, kSnare, 118},     {8, kClosedHat, 90}, {12, kClosedHat, 80}};
const FactoryNote kShuffle[] = {
    {0, kKick, 118},      {0, kClosedHat, 90}, {3, kClosedHat, 70}, {4, kSnare, 112},
    {4, kClosedHat, 88},  {7, kClosedHat, 70}, {8, kKick, 105},     {8, kClosedHat, 90},
    {11, kClosedHat, 70}, {12, kSnare, 112},   {12, kClosedHat, 88}, {15, kClosedHat, 70}};
const FactoryNote kTrap[] = {
    {0, kKick, kAccent},  {0, kClosedHat, 90}, {2, kClosedHat, 70}, {4, kClosedHat, 80},
    {6, kClosedHat, 70},  {7, kKick, kHit},    {8, kSnare, 115},    {8, kClosedHat, 85},
    {10, kKick, 105},     {10, kClosedHat, 70}, {12, kClosedHat, 80}, {13, kClosedHat, 60},
    {14, kClosedHat, 70}, {15, kClosedHat, 60}};
const FactoryNote kTwoStep[] = {{0, kKick, kAccent},  {2, kClosedHat, 80}, {4, kSnare, 115},
                                {6, kClosedHat, 80},  {10, kKick, 105},    {10, kClosedHat, 80},
                                {12, kSnare, 115},    {14, kClosedHat, 80}};
const FactoryNote kLatin[] = {
    {0, kKick, 118},     {0, kClosedHat, 85},  {2, kRim, 95},       {2, kClosedHat, 70},
    {4, kClosedHat, 85}, {5, kRim, 90},        {6, kKick, kHit},    {6, kClosedHat, 70},
    {8, kKick, 110},     {8, kClosedHat, 85},  {9, kRim, 95},       {10, kClosedHat, 70},
    {11, kRim, 90},      {12, kClosedHat, 85}, {14, kKick, kHit},   {14, kClosedHat, 70}};

// ---- bass, one bar, C minor from C2 ----

const uint8_t kC2 = 36, kEb2 = 39, kF2 = 41, kG2 = 43, kBb2 = 46, kC3 = 48;

const FactoryNote kRoots[] = {{0, kC2, 110}, {4, kC2, kHit}, {8, kC2, 105}, {12, kC2, kHit}};
const FactoryNote kOctaves[] = {{0, kC2, 110},  {2, kC3, 95},  {4, kC2, 105}, {6, kC3, 95},
                                {8, kC2, 110},  {10, kC3, 95}, {12, kC2, 105}, {14, kC3, 95}};
const FactoryNote kOffbeat[] = {{2, kC2, 105}, {6, kC2, kHit}, {10, kC2, 105}, {14, kC2, kHit}};
const FactoryNote kWalking[] = {{0, kC2, 105},  {2, kEb2, kHit}, {4, kF2, kHit}, {6, kG2, kHit},
                                {8, kBb2, 105}, {10, kG2, kHit}, {12, kF2, kHit}, {14, kEb2, kHit}};
const FactoryNote kAcid[] = {{0, kC2, 118},  {1, kC2, kGhost}, {3, kC3, kHit}, {4, kC2, 105},
                             {6, kEb2, 95},  {8, kC2, 110},    {9, kC2, kGhost}, {11, kG2, kHit},
                             {12, kC2, 105}, {14, kEb2, 95},   {15, kC3, 85}};
const FactoryNote kDub[] = {{0, kC2, 118}, {10, kEb2, kHit}};
const FactoryNote kDriving[] = {{0, kC2, 112},  {2, kC2, 95},  {4, kC2, 105}, {6, kC2, 95},
                                {8, kC2, 112},  {10, kC2, 95}, {12, kC2, 105}, {14, kC2, 95}};
const FactoryNote kSyncopated[] = {{0, kC2, 112}, {3, kC2, 95}, {6, kEb2, kHit},
                                   {10, kF2, kHit}, {14, kG2, 95}};

// ---- chords, two bars, C minor ----

const uint8_t kC3c = 48, kEb3 = 51, kG3 = 55, kBb3 = 58, kAb2 = 44, kF3 = 53, kD3 = 50;

const FactoryNote kHeld[] = {{0, kC3c, 95}, {0, kEb3, 95}, {0, kG3, 95}};
const FactoryNote kStabs[] = {{0, kC3c, 105},  {0, kEb3, 105},  {0, kG3, 105},
                              {8, kC3c, 95},   {8, kEb3, 95},   {8, kG3, 95},
                              {16, kC3c, 105}, {16, kEb3, 105}, {16, kG3, 105},
                              {24, kC3c, 95},  {24, kEb3, 95},  {24, kG3, 95}};
const FactoryNote kChordOffbeat[] = {{2, kC3c, 100},  {2, kEb3, 100},  {2, kG3, 100},
                                     {10, kC3c, 90},  {10, kEb3, 90},  {10, kG3, 90},
                                     {18, kC3c, 100}, {18, kEb3, 100}, {18, kG3, 100},
                                     {26, kC3c, 90},  {26, kEb3, 90},  {26, kG3, 90}};
const FactoryNote kProgression[] = {{0, kC3c, 100},  {0, kEb3, 100},  {0, kG3, 100},
                                    {8, kAb2, 100},  {8, kC3c, 100},  {8, kEb3, 100},
                                    {16, kEb3, 100}, {16, kG3, 100},  {16, kBb3, 100},
                                    {24, kBb3 - 12, 100}, {24, kD3, 100}, {24, kF3, 100}};
const FactoryNote kSus[] = {{0, kC3c, 100},  {0, kF3, 100},  {0, kG3, 100},
                            {16, kC3c, 95},  {16, kF3, 95},  {16, kG3, 95}};
const FactoryNote kSevenths[] = {{0, kC3c, 100},  {0, kEb3, 100},  {0, kG3, 100}, {0, kBb3, 100},
                                 {16, kAb2, 95},  {16, kC3c, 95},  {16, kEb3, 95}, {16, kG3, 95}};
const FactoryNote kPads[] = {{0, kC3c, 85},  {0, kEb3, 85},  {0, kG3, 85},
                             {16, kAb2, 85}, {16, kC3c, 85}, {16, kEb3, 85}};
const FactoryNote kPumping[] = {
    {0, kC3c, 105},  {0, kEb3, 105},  {4, kC3c, 80},   {4, kEb3, 80},
    {8, kC3c, 105},  {8, kEb3, 105},  {12, kC3c, 80},  {12, kEb3, 80},
    {16, kC3c, 105}, {16, kEb3, 105}, {20, kC3c, 80},  {20, kEb3, 80},
    {24, kC3c, 105}, {24, kEb3, 105}, {28, kC3c, 80},  {28, kEb3, 80}};

// ---- arp, one bar, C minor from C4 ----

const uint8_t kC4 = 60, kEb4 = 63, kG4 = 67, kC5 = 72, kG5 = 79;

const FactoryNote kArpUp[] = {{0, kC4, 105},  {1, kEb4, 95}, {2, kG4, 95},   {3, kC5, 105},
                              {4, kC4, 100},  {5, kEb4, 90}, {6, kG4, 90},   {7, kC5, 100},
                              {8, kC4, 105},  {9, kEb4, 95}, {10, kG4, 95},  {11, kC5, 105},
                              {12, kC4, 100}, {13, kEb4, 90}, {14, kG4, 90}, {15, kC5, 100}};
const FactoryNote kArpDown[] = {{0, kC5, 105},  {1, kG4, 95},  {2, kEb4, 95},  {3, kC4, 105},
                                {4, kC5, 100},  {5, kG4, 90},  {6, kEb4, 90},  {7, kC4, 100},
                                {8, kC5, 105},  {9, kG4, 95},  {10, kEb4, 95}, {11, kC4, 105},
                                {12, kC5, 100}, {13, kG4, 90}, {14, kEb4, 90}, {15, kC4, 100}};
const FactoryNote kArpUpDown[] = {{0, kC4, 105},  {1, kEb4, 95},  {2, kG4, 95},  {3, kC5, 105},
                                  {4, kG4, 95},   {5, kEb4, 95},  {6, kC4, 100}, {7, kEb4, 90},
                                  {8, kG4, 95},   {9, kC5, 105},  {10, kG4, 95}, {11, kEb4, 95},
                                  {12, kC4, 100}, {13, kEb4, 90}, {14, kG4, 95}, {15, kC5, 105}};
const FactoryNote kArpOctaves[] = {{0, kC4, 105},  {2, kC5, 95},  {4, kC4, 100},  {6, kC5, 95},
                                   {8, kC4, 105},  {10, kC5, 95}, {12, kC4, 100}, {14, kC5, 95}};
const FactoryNote kArpWide[] = {{0, kC4, 105},  {2, kG4, 95},  {4, kC5, 100},  {6, kG5, 95},
                                {8, kC5, 105},  {10, kG4, 95}, {12, kC4, 100}, {14, kG4, 95}};
const FactoryNote kArpThirds[] = {{0, kC4, 105},  {1, kEb4, 90}, {2, kC4, 95},  {3, kG4, 100},
                                  {4, kC4, 100},  {5, kC5, 105}, {6, kC4, 95},  {7, kG4, 100},
                                  {8, kC4, 105},  {9, kEb4, 90}, {10, kC4, 95}, {11, kG4, 100},
                                  {12, kC4, 100}, {13, kC5, 105}, {14, kG4, 95}, {15, kEb4, 90}};
const FactoryNote kArpGallop[] = {{0, kC4, 110}, {1, kC4, kGhost}, {2, kEb4, 95},
                                  {4, kG4, 105}, {5, kG4, kGhost}, {6, kC5, 95},
                                  {8, kC4, 110}, {9, kC4, kGhost}, {10, kEb4, 95},
                                  {12, kG4, 105}, {13, kG4, kGhost}, {14, kC5, 95}};
const FactoryNote kArpSparse[] = {{0, kC4, 105}, {4, kG4, 95}, {8, kC5, 100}, {12, kEb4, 95}};

// ---- fills, one bar ----

const FactoryNote kSnareRoll[] = {{8, kSnare, 80},  {10, kSnare, 90}, {12, kSnare, 100},
                                  {13, kSnare, 105}, {14, kSnare, 112}, {15, kSnare, kAccent}};
const FactoryNote kTomFall[] = {{8, kHighTom, 110}, {10, kHighTom, 100}, {12, kMidTom, 110},
                                {13, kMidTom, 100}, {14, kLowTom, 115},  {15, kLowTom, kAccent}};
const FactoryNote kCrashHit[] = {{0, kCrash, kAccent}, {0, kKick, 110}, {8, kSnare, 110}};
const FactoryNote kBuild[] = {{8, kSnare, 70},  {10, kSnare, 85}, {12, kSnare, 95},
                              {13, kSnare, 100}, {14, kSnare, 110}, {15, kSnare, kAccent}};
const FactoryNote kStutter[] = {{12, kSnare, 105}, {13, kSnare, kGhost}, {14, kSnare, 110},
                                {15, kSnare, kGhost}};
const FactoryNote kFlam[] = {{4, kRim, 70},  {4, kSnare, 110}, {12, kRim, 70}, {12, kSnare, 115}};
const FactoryNote kDrop[] = {{15, kCrash, kAccent}};
const FactoryNote kRollOut[] = {{8, kHighTom, 90},  {9, kHighTom, 80},  {10, kMidTom, 95},
                                {11, kMidTom, 85},  {12, kLowTom, 100}, {13, kLowTom, 90},
                                {14, kSnare, 110},  {15, kSnare, kAccent}};

#define GX_ENTRY(name, label, length) \
  { label, length, name, static_cast<uint8_t>(sizeof(name) / sizeof(name[0])) }

const FactoryEntry kBank[kNumFactoryRoles][kFactoryPatternsPerRole] = {
    {GX_ENTRY(kFourFloor, "FOUR FLOOR", 16), GX_ENTRY(kBackbeat, "BACKBEAT", 16),
     GX_ENTRY(kBreakbeat, "BREAKBEAT", 16), GX_ENTRY(kHalfTime, "HALF TIME", 16),
     GX_ENTRY(kShuffle, "SHUFFLE", 16), GX_ENTRY(kTrap, "TRAP", 16),
     GX_ENTRY(kTwoStep, "TWO STEP", 16), GX_ENTRY(kLatin, "LATIN", 16)},
    {GX_ENTRY(kRoots, "ROOTS", 16), GX_ENTRY(kOctaves, "OCTAVES", 16),
     GX_ENTRY(kOffbeat, "OFFBEAT", 16), GX_ENTRY(kWalking, "WALKING", 16),
     GX_ENTRY(kAcid, "ACID", 16), GX_ENTRY(kDub, "DUB", 16),
     GX_ENTRY(kDriving, "DRIVING", 16), GX_ENTRY(kSyncopated, "SYNCOPATED", 16)},
    {GX_ENTRY(kHeld, "HELD", 32), GX_ENTRY(kStabs, "STABS", 32),
     GX_ENTRY(kChordOffbeat, "OFFBEAT", 32), GX_ENTRY(kProgression, "PROGRESSION", 32),
     GX_ENTRY(kSus, "SUS 4", 32), GX_ENTRY(kSevenths, "SEVENTHS", 32),
     GX_ENTRY(kPads, "PADS", 32), GX_ENTRY(kPumping, "PUMPING", 32)},
    {GX_ENTRY(kArpUp, "UP", 16), GX_ENTRY(kArpDown, "DOWN", 16),
     GX_ENTRY(kArpUpDown, "UP DOWN", 16), GX_ENTRY(kArpOctaves, "OCTAVES", 16),
     GX_ENTRY(kArpWide, "WIDE", 16), GX_ENTRY(kArpThirds, "THIRDS", 16),
     GX_ENTRY(kArpGallop, "GALLOP", 16), GX_ENTRY(kArpSparse, "SPARSE", 16)},
    {GX_ENTRY(kSnareRoll, "SNARE ROLL", 16), GX_ENTRY(kTomFall, "TOM FALL", 16),
     GX_ENTRY(kCrashHit, "CRASH", 16), GX_ENTRY(kBuild, "BUILD", 16),
     GX_ENTRY(kStutter, "STUTTER", 16), GX_ENTRY(kFlam, "FLAM", 16),
     GX_ENTRY(kDrop, "DROP", 16), GX_ENTRY(kRollOut, "ROLL OUT", 16)},
};

#undef GX_ENTRY

// The expanded bank, built once. Static rather than built per call: currentPattern() hands
// back a pointer that the caller reads from, and several tracks are read in one frame, so a
// single shared scratch pattern would be overwritten under the previous reader's feet.
Pattern gBank[kNumFactoryRoles][kFactoryPatternsPerRole];
Pattern gEmpty;
bool gBuilt = false;

void expand(const FactoryEntry& entry, Pattern& out) {
  initPattern(out);
  out.length = entry.length <= kMaxSteps ? entry.length : kMaxSteps;
  for (uint8_t i = 0; i < entry.count; ++i) {
    const FactoryNote& n = entry.notes[i];
    if (n.step >= out.length) continue;  // a build with a short pattern keeps what fits
    Step& step = out.steps[n.step];
    if (step.noteCount >= kMaxStepNotes) continue;  // a chord holds four at most
    step.active = 1;
    step.notes[step.noteCount++] = n.note;
    step.velocity = n.velocity;  // the step's velocity, which its whole chord plays at
  }
}

}  // namespace

static_assert(kFactoryTrackGroup == kTracksPerMidiPort,
              "the bank's roles repeat over the same eight tracks a page and a port do");
static_assert(kFirstFactoryPattern >= 1, "a build needs at least one pattern of its own");

void initFactoryPatterns() {
  if (gBuilt) return;
  initPattern(gEmpty);
  for (uint8_t role = 0; role < kNumFactoryRoles; ++role) {
    for (uint8_t slot = 0; slot < kFactoryPatternsPerRole; ++slot) {
      expand(kBank[role][slot], gBank[role][slot]);
    }
  }
  gBuilt = true;
}

uint8_t factoryRole(uint8_t track) {
  if (track >= kNumTracks) return kNumFactoryRoles;
  const uint8_t place = static_cast<uint8_t>(track % kFactoryTrackGroup);
  return place < kNumFactoryRoles ? place : static_cast<uint8_t>(kNumFactoryRoles);
}

const Pattern* factoryPattern(uint8_t track, uint8_t pattern) {
  initFactoryPatterns();
  const uint8_t role = factoryRole(track);
  if (role >= kNumFactoryRoles || !isFactoryPattern(pattern)) return &gEmpty;
  const uint8_t slot = static_cast<uint8_t>(pattern - kFirstFactoryPattern);
  return slot < kFactoryPatternsPerRole ? &gBank[role][slot] : &gEmpty;
}

const char* factoryPatternName(uint8_t track, uint8_t pattern) {
  const uint8_t role = factoryRole(track);
  if (role >= kNumFactoryRoles || !isFactoryPattern(pattern)) return "";
  const uint8_t slot = static_cast<uint8_t>(pattern - kFirstFactoryPattern);
  return slot < kFactoryPatternsPerRole ? kBank[role][slot].name : "";
}

}  // namespace gx
