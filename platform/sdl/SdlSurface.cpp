#include "sdl/SdlSurface.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui/Font3x5.h"
#include "ui/Mode.h"
#include "ui/UiController.h"  // ModeId and the R button names the mode bar stands on

namespace gx {

namespace {

const SDL_Color kBackground = {16, 16, 19, 255};
const SDL_Color kPanel = {30, 30, 35, 255};
const SDL_Color kLabel = {140, 140, 150, 255};
const SDL_Color kStatus = {225, 225, 230, 255};
const SDL_Color kHint = {95, 95, 105, 255};
const SDL_Color kPressedRing = {235, 235, 240, 255};
const Rgb kRubber = {44, 44, 50};  // colour of an unlit pad
const Uint8 kGlowAlpha = 55;
const int kGlowSize = 5;
const int kPressedRingWidth = 3;
const int kPanelRadius = 14;
const int kLabelLineGap = 4;
const SDL_Color kPadTextLight = {230, 230, 235, 255};
const SDL_Color kPadTextDark = {20, 20, 24, 255};
const int kPadLabelLineGap = 3;
const SDL_Color kModeIdle = {38, 38, 45, 255};
const SDL_Color kModeHover = {52, 52, 61, 255};
const SDL_Color kModeActive = {236, 238, 243, 255};
const SDL_Color kModeActiveText = {18, 18, 22, 255};
const SDL_Color kFaderSlot = {10, 10, 12, 255};
const SDL_Color kFaderCap = {170, 170, 178, 255};
const SDL_Color kFaderLine = {40, 40, 46, 255};
const int kFaderCapRadius = 3;
const int kFaderTickWidth = 8;
const int kWheelStep = kFaderMax / 32;
const int kFineWheelStep = kFaderMax / 256;  // with Ctrl: under 1 BPM when setting the tempo
// Where the faders start: about MIDI volume 100, a usual mixer starting point.
const uint16_t kDefaultFaderValue = kFaderMax * 100 / 127;

void setColor(SDL_Renderer* r, const SDL_Color& c) {
  SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

Uint8 addSaturate(Uint8 a, Uint8 b) {
  const int sum = a + b;
  return static_cast<Uint8>(sum > 255 ? 255 : sum);
}

SDL_Rect inflate(const SDL_Rect& rect, int by) {
  SDL_Rect out = {rect.x - by, rect.y - by, rect.w + 2 * by, rect.h + 2 * by};
  return out;
}

// Fills a rectangle with rounded corners, one horizontal span per row.
void fillRoundRect(SDL_Renderer* r, const SDL_Rect& rect, int radius) {
  for (int row = 0; row < rect.h; ++row) {
    int dy = -1;
    if (row < radius) {
      dy = radius - 1 - row;
    } else if (row >= rect.h - radius) {
      dy = row - (rect.h - radius);
    }
    int inset = 0;
    if (dy >= 0) {
      const double y = dy + 0.5;
      inset = radius - static_cast<int>(std::sqrt(radius * radius - y * y) + 0.5);
    }
    SDL_Rect span = {rect.x + inset, rect.y + row, rect.w - 2 * inset, 1};
    SDL_RenderFillRect(r, &span);
  }
}

void drawControl(SDL_Renderer* r, const SDL_Rect& rect, Rgb led, bool pressed, int radius) {
  if (isLit(led)) {
    SDL_SetRenderDrawColor(r, led.r, led.g, led.b, kGlowAlpha);
    fillRoundRect(r, inflate(rect, kGlowSize), radius + kGlowSize);
  }

  SDL_Rect face = rect;
  if (pressed) {
    setColor(r, kPressedRing);
    fillRoundRect(r, rect, radius);
    face = inflate(rect, -kPressedRingWidth);
    radius -= kPressedRingWidth;
  }

  SDL_SetRenderDrawColor(r, addSaturate(kRubber.r, led.r), addSaturate(kRubber.g, led.g),
                         addSaturate(kRubber.b, led.b), 255);
  fillRoundRect(r, face, radius);
}

// At 3 pixels wide a '#' reads as an H, so note names like C# get a 5-wide sharp.
const int kSharpWidth = 5;
const uint8_t kSharpRows[kGlyphHeight] = {012, 037, 012, 037, 012};  // bit 4 = left column

int charWidth(char ch) { return ch == '#' ? kSharpWidth : kGlyphWidth; }

// The mode bar. Each button is the R button that opens that mode, with or without Shift, so
// clicking one presses what a player presses - the shortcut cannot behave differently from
// the hardware because it goes through the same events.
struct ModeButton {
  const char* label;
  uint8_t button;  // R1..R8, 0-based
  bool shift;
};
const ModeButton kModeButtons[] = {
    {"PROJECT", kButtonClear, true},        {"SETTINGS", kButtonProbability, true},
    {"SONG", kButtonPlay, true},            {"SCENE", kButtonPattern, true},
    {"PATTERN", kButtonPattern, false},     {"NOTE", kButtonNote, false},
    {"SCALE", kButtonNote, true},           {"PARAM", kButtonParams, false},
    {"PRESET", kButtonParams, true},        {"PROB", kButtonProbability, false},
};
const uint8_t kNumModeButtons = sizeof(kModeButtons) / sizeof(kModeButtons[0]);

// Which mode each button opens, in ModeId order, so the open one can be lit.
const uint8_t kModeOfButton[kNumModeButtons] = {
    kModeProject, kModeGlobal, kModeArrangement, kModeScene, kModePattern,
    kModeNote,    kModeScale,  kModeStepParams,  kModePreset, kModeProbability,
};

ControlEvent makeControl(uint8_t group, uint8_t index, bool pressed) {
  ControlEvent event;
  std::memset(&event, 0, sizeof(event));
  event.group = group;
  event.index = index;
  event.pressed = pressed;
  return event;
}

int textWidth(const char* text, int scale) {
  int width = 0;
  for (; *text; ++text) width += (charWidth(*text) + 1) * scale;
  return width > 0 ? width - scale : 0;
}

void drawText(SDL_Renderer* r, int x, int y, const char* text, int scale) {
  for (; *text; x += (charWidth(*text) + 1) * scale, ++text) {
    char ch = *text;
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (ch < kFontFirstChar || ch > kFontLastChar) continue;

    const uint16_t glyph = kFont3x5[ch - kFontFirstChar];
    for (int row = 0; row < kGlyphHeight; ++row) {
      for (int col = 0; col < charWidth(ch); ++col) {
        const bool on =
            ch == '#' ? (kSharpRows[row] >> (kSharpWidth - 1 - col)) & 1
                      : (glyph >> ((kGlyphHeight - 1 - row) * kGlyphWidth + (kGlyphWidth - 1 - col))) & 1;
        if (on) {
          SDL_Rect px = {x + col * scale, y + row * scale, scale, scale};
          SDL_RenderFillRect(r, &px);
        }
      }
    }
  }
}

// Prints a pad label centred on the pad, one line per '\n': dark on bright pads, light on
// dim ones.
void drawPadLabel(SDL_Renderer* r, const SDL_Rect& rect, Rgb led, const char* text) {
  if (!text || !*text) return;
  const int luma = 2126 * addSaturate(kRubber.r, led.r) + 7152 * addSaturate(kRubber.g, led.g) +
                   722 * addSaturate(kRubber.b, led.b);
  setColor(r, luma > 140 * 10000 ? kPadTextDark : kPadTextLight);
  int lines = 1;
  for (const char* c = text; *c; ++c) {
    if (*c == '\n') ++lines;
  }
  const int lineHeight = SdlSurface::kTextHeight + kPadLabelLineGap;
  int y = rect.y + (rect.h - lines * lineHeight + kPadLabelLineGap) / 2;
  char line[kPadLabelLineChars + 1];
  for (const char* start = text;; y += lineHeight) {
    const char* end = start;
    while (*end && *end != '\n') ++end;
    size_t len = static_cast<size_t>(end - start);
    if (len > kPadLabelLineChars) len = kPadLabelLineChars;
    std::memcpy(line, start, len);
    line[len] = '\0';
    drawText(r, rect.x + (rect.w - textWidth(line, SdlSurface::kTextScale)) / 2, y, line,
             SdlSurface::kTextScale);
    if (!*end) break;
    start = end + 1;
  }
}

// The knob's pointer sweeps 270 degrees, from bottom left at 0 to bottom right at the top
// of its range.
void knobPointer(int value, double& dx, double& dy) {
  const double angle = (-135.0 + 270.0 * value / kFaderMax) * 3.14159265358979 / 180.0;
  dx = std::sin(angle);
  dy = -std::cos(angle);
}

// Fader position for a pointer height: the cap's centre runs from the top of the travel
// (kFaderMax) down to the bottom (0).
int faderValueAtY(const SDL_Rect& rect, int y) {
  const int travel = rect.h - SdlSurface::kFaderCapHeight;
  if (travel <= 0) return 0;
  const int top = rect.y + SdlSurface::kFaderCapHeight / 2;
  return (top + travel - y) * kFaderMax / travel;
}

bool keyToControl(SDL_Keycode key, ControlEvent& out) {
  out.value = 0;
  out.initial = false;
  out.velocity = 0;  // a mouse cannot say how hard
  if (key >= SDLK_F1 && key <= SDLK_F8) {
    out.group = kGroupRight;
    out.index = static_cast<uint8_t>(key - SDLK_F1);
    return true;
  }
  if (key == SDLK_SPACE) {
    out.group = kGroupRight;
    out.index = kNumRightButtons - 1;
    return true;
  }
  if (key >= SDLK_1 && key <= SDLK_8) {
    out.group = kGroupBottom;
    out.index = static_cast<uint8_t>(key - SDLK_1);
    return true;
  }
  if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
    out.group = kGroupShift;
    out.index = 0;
    return true;
  }
  return false;
}

}  // namespace

SdlSurface::SdlSurface()
    : window_(NULL),
      renderer_(NULL),
      videoInitialized_(false),
      vsync_(false),
      quitRequested_(false),
      mouseHeld_(false),
      shiftPressed_(false),
      queuedCount_(0),
      queuedNext_(0),
      activeMode_(0xFF) {
  std::memset(&mouseControl_, 0, sizeof(mouseControl_));
  frame_.clear();
  status_[0] = '\0';
  for (uint8_t i = 0; i < kNumRightButtons; ++i) rightLabels_[i] = "";
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) bottomLabels_[i] = NULL;
  for (uint8_t i = 0; i < kNumPads; ++i) padLabels_[i] = NULL;
  labelsShifted_ = false;
  std::memset(padPressed_, 0, sizeof(padPressed_));
  std::memset(rightPressed_, 0, sizeof(rightPressed_));
  std::memset(bottomPressed_, 0, sizeof(bottomPressed_));
  for (uint8_t i = 0; i < kNumTrackFaders; ++i) faderValues_[i] = kDefaultFaderValue;
  masterFaderValue_ = kDefaultFaderValue;
  for (uint8_t i = 0; i < kNumKnobs; ++i) knobValues_[i] = kFaderMax / 2;  // knobs start centred
  for (uint8_t i = 0; i < kNumMixStrips; ++i) mixFaderValues_[i] = kDefaultFaderValue;
  mixMasterValue_ = kDefaultFaderValue;
  std::memset(mixButtonPressed_, 0, sizeof(mixButtonPressed_));
  std::memset(mixSidePressed_, 0, sizeof(mixSidePressed_));
  faderSync_ = 0;
  draggingFader_ = false;
  std::memset(&dragFader_, 0, sizeof(dragFader_));
  draggingKnob_ = false;
  std::memset(&dragKnob_, 0, sizeof(dragKnob_));
  dragStartY_ = 0;
  dragStartValue_ = 0;
  mouseX_ = -1;
  mouseY_ = -1;
}

SdlSurface::~SdlSurface() {
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  if (videoInitialized_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool SdlSurface::init(const char* title) {
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
    return false;
  }
  videoInitialized_ = true;

  window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
                             kWindowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window_) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, 0);
  if (!renderer_) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }
  // Fixed logical layout; SDL scales drawing and mouse coordinates to the window size.
  SDL_RenderSetLogicalSize(renderer_, kWindowWidth, kWindowHeight);
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

  SDL_RendererInfo info;
  vsync_ = SDL_GetRendererInfo(renderer_, &info) == 0 && (info.flags & SDL_RENDERER_PRESENTVSYNC);
  return true;
}

bool SdlSurface::pollEvent(ControlEvent& event) {
  // Like a hardware scan at power-up, report every fader and knob position once before
  // anything else.
  if (syncControl(faderSync_, event)) {
    event.pressed = false;
    event.initial = true;
    event.velocity = 0;
    event.value = faderValue(event);
    ++faderSync_;
    return true;
  }

  // A mode button's presses, one per poll, ahead of the mouse and keyboard: they are a press
  // and a release that have to reach the app in order.
  if (takeQueued(event)) return true;

  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        quitRequested_ = true;
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if (e.key.keysym.sym == SDLK_ESCAPE) {
          quitRequested_ = true;
        } else if (!e.key.repeat && keyToControl(e.key.keysym.sym, event)) {
          event.pressed = (e.type == SDL_KEYDOWN);
          setPressed(event);
          return true;
        }
        break;
      case SDL_MOUSEBUTTONDOWN: {
        ControlEvent hit;
        mouseX_ = e.button.x;
        mouseY_ = e.button.y;
        const int mode = e.button.button == SDL_BUTTON_LEFT ? modeAt(e.button.x, e.button.y) : -1;
        if (mode >= 0) {
          // Queue the presses this mode is opened with. Shift wraps the R button when the
          // mode needs it, exactly as a hand would hold it.
          // Not while the last one is still coming out: refilling would drop its tail, and
          // the tail is the Shift release. The app drains the queue every frame, so this
          // only ever declines a second click inside one frame.
          if (queuedNext_ < queuedCount_) break;
          const ModeButton& picked = kModeButtons[mode];
          queuedCount_ = queuedNext_ = 0;
          if (picked.shift) queued_[queuedCount_++] = makeControl(kGroupShift, 0, true);
          queued_[queuedCount_++] = makeControl(kGroupRight, picked.button, true);
          queued_[queuedCount_++] = makeControl(kGroupRight, picked.button, false);
          if (picked.shift) queued_[queuedCount_++] = makeControl(kGroupShift, 0, false);
          // Hand out the first one now: this poll found the click, and returning nothing
          // would leave the whole press a poll behind the click that made it.
          takeQueued(event);
          return true;
        }
        if (faderAt(e.button.x, e.button.y, hit)) {
          // Left-click grabs a fader: the cap jumps to the pointer and follows it while dragged.
          if (e.button.button != SDL_BUTTON_LEFT || mouseHeld_ || draggingFader_) break;
          draggingFader_ = true;
          dragFader_ = hit;
          if (moveFader(hit, faderValueAtY(faderRect(hit.group, hit.index), e.button.y), event)) {
            return true;
          }
          break;
        }
        if (knobAt(e.button.x, e.button.y, hit)) {
          // A knob turns by how far the pointer moves, like the real thing, so it doesn't
          // jump when it is grabbed.
          if (e.button.button != SDL_BUTTON_LEFT || mouseHeld_ || draggingKnob_) break;
          draggingKnob_ = true;
          dragKnob_ = hit;
          dragStartY_ = e.button.y;
          dragStartValue_ = faderValue(hit);
          break;
        }
        if (!hitTest(e.button.x, e.button.y, hit)) break;
        if (e.button.button == SDL_BUTTON_RIGHT) {
          // Right-click latches a control down until it is right-clicked again, so chords
          // and other combinations can be played with a single mouse.
          hit.pressed = !isPressed(hit);
          setPressed(hit);
          event = hit;
          return true;
        }
        if (e.button.button == SDL_BUTTON_LEFT && !mouseHeld_ && !isPressed(hit)) {
          mouseHeld_ = true;
          mouseControl_ = hit;
          event = hit;
          setPressed(event);
          return true;
        }
        break;
      }
      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT && draggingKnob_) draggingKnob_ = false;
        if (e.button.button == SDL_BUTTON_LEFT && draggingFader_) {
          draggingFader_ = false;
          break;
        }
        if (e.button.button == SDL_BUTTON_LEFT && mouseHeld_) {
          mouseHeld_ = false;
          mouseControl_.pressed = false;
          event = mouseControl_;
          setPressed(event);
          return true;
        }
        break;
      case SDL_MOUSEMOTION:
        mouseX_ = e.motion.x;
        mouseY_ = e.motion.y;
        if (draggingFader_ &&
            moveFader(dragFader_, faderValueAtY(faderRect(dragFader_.group, dragFader_.index),
                                                e.motion.y),
                      event)) {
          return true;
        }
        if (draggingKnob_) {
          const int moved = (dragStartY_ - e.motion.y) * kFaderMax / SdlSurface::kKnobDragRange;
          if (moveFader(dragKnob_, dragStartValue_ + moved, event)) return true;
        }
        break;
      case SDL_MOUSEWHEEL: {
        ControlEvent hit;
        if (draggingFader_ || draggingKnob_) break;
        if (!faderAt(mouseX_, mouseY_, hit) && !knobAt(mouseX_, mouseY_, hit)) break;
        int steps = e.wheel.y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) steps = -steps;
        const int step = (SDL_GetModState() & KMOD_CTRL) ? kFineWheelStep : kWheelStep;
        if (moveFader(hit, faderValue(hit) + steps * step, event)) return true;
        break;
      }
    }
  }
  return false;
}

void SdlSurface::show(const LedFrame& frame) { frame_ = frame; }

void SdlSurface::setStatus(const char* text) {
  std::snprintf(status_, sizeof(status_), "%s", text);
}

void SdlSurface::setRightLabel(uint8_t index, const char* text) {
  if (index < kNumRightButtons) rightLabels_[index] = text ? text : "";
}

void SdlSurface::setBottomLabel(uint8_t index, const char* text) {
  if (index < kNumBottomButtons) bottomLabels_[index] = text;
}

void SdlSurface::setPadLabel(uint8_t index, const char* text) {
  if (index < kNumPads) padLabels_[index] = text;
}

void SdlSurface::setFaderPosition(uint8_t group, uint8_t index, uint16_t value) {
  ControlEvent fader;
  std::memset(&fader, 0, sizeof(fader));
  fader.group = group;
  fader.index = index;
  faderValue(fader) = value > kFaderMax ? kFaderMax : value;
}

SDL_Rect SdlSurface::padRect(uint8_t index) {
  const int row = index / kGridCols;
  const int col = index % kGridCols;
  SDL_Rect rect = {kGridX + col * kPitch, kGridY + row * kPitch, kPadSize, kPadSize};
  return rect;
}

SDL_Rect SdlSurface::rightRect(uint8_t index) {
  const int offset = (kPadSize - kButtonSize) / 2;
  SDL_Rect rect = {kRightX, kGridY + index * kPitch + offset, kButtonSize, kButtonSize};
  return rect;
}

SDL_Rect SdlSurface::bottomRect(uint8_t index) {
  const int offset = (kPadSize - kButtonSize) / 2;
  SDL_Rect rect = {kGridX + index * kPitch + offset, kBottomY, kButtonSize, kButtonSize};
  return rect;
}

// Bottom-right corner: in the R column, on the B row.
SDL_Rect SdlSurface::shiftRect() {
  SDL_Rect rect = {kRightX, kBottomY, kButtonSize, kButtonSize};
  return rect;
}

// Mixer strips are kStripPitch apart; knobs and A1/A2 buttons run row by row from the top
// left, and the side buttons sit beside the knob rows and the A1 row.
// Knob: a dark body with scale marks around it and a pointer showing the position.
void SdlSurface::drawKnob(const SDL_Rect& rect, uint16_t value, bool active) {
  SDL_Renderer* r = renderer_;
  const int cx = rect.x + rect.w / 2;
  const int cy = rect.y + rect.h / 2;
  const int radius = rect.w / 2;

  setColor(r, kHint);  // five marks around the sweep
  for (int tick = 0; tick <= 4; ++tick) {
    double dx = 0, dy = 0;
    knobPointer(tick * kFaderMax / 4, dx, dy);
    const SDL_Rect mark = {cx + static_cast<int>(dx * (radius + 5)) - 1,
                           cy + static_cast<int>(dy * (radius + 5)) - 1, 3, 3};
    SDL_RenderFillRect(r, &mark);
  }

  setColor(r, kFaderSlot);
  fillRoundRect(r, rect, radius);
  setColor(r, active ? kPressedRing : kFaderCap);
  const SDL_Rect body = inflate(rect, -3);
  fillRoundRect(r, body, body.w / 2);

  double dx = 0, dy = 0;
  knobPointer(value, dx, dy);
  setColor(r, kFaderLine);  // the pointer, drawn a few pixels wide
  for (int offset = -1; offset <= 1; ++offset) {
    SDL_RenderDrawLine(r, cx + static_cast<int>(dx * radius * 0.30) - static_cast<int>(dy * offset),
                       cy + static_cast<int>(dy * radius * 0.30) + static_cast<int>(dx * offset),
                       cx + static_cast<int>(dx * radius * 0.86) - static_cast<int>(dy * offset),
                       cy + static_cast<int>(dy * radius * 0.86) + static_cast<int>(dx * offset));
  }
}

SDL_Rect SdlSurface::knobRect(uint8_t index) {
  const int row = index / kNumMixStrips;
  const int strip = index % kNumMixStrips;
  SDL_Rect rect = {kMixX + strip * kStripPitch + (kStripWidth - kKnobSize) / 2,
                   kMixY + row * kKnobPitch, kKnobSize, kKnobSize};
  return rect;
}

SDL_Rect SdlSurface::mixButtonRect(uint8_t index) {
  const int row = index / kNumMixStrips;
  const int strip = index % kNumMixStrips;
  SDL_Rect rect = {kMixX + strip * kStripPitch + (kStripWidth - kMixButtonWidth) / 2,
                   kMixButtonsY + row * kMixButtonPitch, kMixButtonWidth, kMixButtonHeight};
  return rect;
}

SDL_Rect SdlSurface::mixSideRect(uint8_t index) {
  const int x = kMixSideX + (kStripWidth - kMixButtonWidth) / 2;
  const int y = index < kKnobRows ? kMixY + index * kKnobPitch + (kKnobSize - kMixButtonHeight) / 2
                                  : kMixButtonsY;
  SDL_Rect rect = {x, y, kMixButtonWidth, kMixButtonHeight};
  return rect;
}

SDL_Rect SdlSurface::faderRect(uint8_t group, uint8_t index) {
  if (group == kGroupMixFader) {
    SDL_Rect rect = {kMixX + index * kStripPitch + (kStripWidth - kButtonSize) / 2, kMixFaderY,
                     kButtonSize, kMixFaderHeight};
    return rect;
  }
  if (group == kGroupMixMaster) {
    SDL_Rect rect = {kMixSideX + (kStripWidth - kButtonSize) / 2, kMixMasterY, kButtonSize,
                     kMixMasterHeight};
    return rect;
  }
  const int x = group == kGroupMasterFader
                    ? kRightX
                    : kGridX + index * kPitch + (kPadSize - kButtonSize) / 2;
  SDL_Rect rect = {x, kFaderY, kButtonSize, kFaderHeight};
  return rect;
}

bool SdlSurface::faderAt(int x, int y, ControlEvent& out) const {
  const SDL_Point point = {x, y};
  out.pressed = false;
  out.value = 0;
  out.initial = false;
  out.velocity = 0;  // a mouse cannot say how hard
  const uint8_t groups[4] = {kGroupFader, kGroupMasterFader, kGroupMixFader, kGroupMixMaster};
  const uint8_t counts[4] = {kNumTrackFaders, 1, kNumMixStrips, 1};
  for (uint8_t g = 0; g < 4; ++g) {
    for (uint8_t index = 0; index < counts[g]; ++index) {
      const SDL_Rect rect = faderRect(groups[g], index);
      if (SDL_PointInRect(&point, &rect)) {
        out.group = groups[g];
        out.index = index;
        return true;
      }
    }
  }
  return false;
}

bool SdlSurface::knobAt(int x, int y, ControlEvent& out) const {
  const SDL_Point point = {x, y};
  out.pressed = false;
  out.value = 0;
  out.initial = false;
  out.velocity = 0;  // a mouse cannot say how hard
  for (uint8_t i = 0; i < kNumKnobs; ++i) {
    const SDL_Rect rect = knobRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupKnob;
      out.index = i;
      return true;
    }
  }
  return false;
}

// The start-up scan, in order: the track faders, the master, the mixer's knobs, its faders
// and its master.
bool SdlSurface::syncControl(uint8_t i, ControlEvent& out) const {
  const uint8_t groups[5] = {kGroupFader, kGroupMasterFader, kGroupKnob, kGroupMixFader,
                             kGroupMixMaster};
  const uint8_t counts[5] = {kNumTrackFaders, 1, kNumKnobs, kNumMixStrips, 1};
  uint8_t first = 0;
  for (uint8_t g = 0; g < 5; ++g) {
    if (i < first + counts[g]) {
      out.group = groups[g];
      out.index = static_cast<uint8_t>(i - first);
      return true;
    }
    first = static_cast<uint8_t>(first + counts[g]);
  }
  return false;
}

uint16_t& SdlSurface::faderValue(const ControlEvent& fader) {
  switch (fader.group) {
    case kGroupFader:
      if (fader.index < kNumTrackFaders) return faderValues_[fader.index];
      break;
    case kGroupKnob:
      if (fader.index < kNumKnobs) return knobValues_[fader.index];
      break;
    case kGroupMixFader:
      if (fader.index < kNumMixStrips) return mixFaderValues_[fader.index];
      break;
    case kGroupMixMaster:
      return mixMasterValue_;
    default:
      break;
  }
  return masterFaderValue_;
}

bool SdlSurface::moveFader(const ControlEvent& fader, int value, ControlEvent& event) {
  if (value < 0) value = 0;
  if (value > kFaderMax) value = kFaderMax;
  uint16_t& current = faderValue(fader);
  if (current == value) return false;
  current = static_cast<uint16_t>(value);
  event = fader;
  event.pressed = false;
  event.value = current;
  return true;
}

// The bar spans the whole window, each button an equal share of it.
// The next press a mode button queued, if any. Empties the queue as it goes.
bool SdlSurface::takeQueued(ControlEvent& event) {
  if (queuedNext_ >= queuedCount_) return false;
  event = queued_[queuedNext_++];
  if (queuedNext_ == queuedCount_) queuedCount_ = queuedNext_ = 0;
  setPressed(event);
  return true;
}

SDL_Rect SdlSurface::modeRect(uint8_t index) {
  const int span = kWindowWidth - 2 * kMargin;
  const int pitch = span / kNumModeButtons;
  const SDL_Rect rect = {kMargin + pitch * index, kModeBarY, pitch - kModeButtonGap,
                         kModeBarHeight};
  return rect;
}

int SdlSurface::modeAt(int x, int y) const {
  for (uint8_t i = 0; i < kNumModeButtons; ++i) {
    const SDL_Rect rect = modeRect(i);
    if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) return i;
  }
  return -1;
}

bool SdlSurface::hitTest(int x, int y, ControlEvent& out) const {
  const SDL_Point point = {x, y};
  out.pressed = true;
  out.value = 0;
  out.initial = false;
  out.velocity = 0;  // a mouse cannot say how hard
  for (uint8_t i = 0; i < kNumPads; ++i) {
    const SDL_Rect rect = padRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupPad;
      out.index = i;
      return true;
    }
  }
  for (uint8_t i = 0; i < kNumRightButtons; ++i) {
    const SDL_Rect rect = rightRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupRight;
      out.index = i;
      return true;
    }
  }
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) {
    const SDL_Rect rect = bottomRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupBottom;
      out.index = i;
      return true;
    }
  }
  const SDL_Rect shift = shiftRect();
  if (SDL_PointInRect(&point, &shift)) {
    out.group = kGroupShift;
    out.index = 0;
    return true;
  }
  for (uint8_t i = 0; i < kNumMixButtons; ++i) {
    const SDL_Rect rect = mixButtonRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupMixButton;
      out.index = i;
      return true;
    }
  }
  for (uint8_t i = 0; i < kNumMixSideButtons; ++i) {
    const SDL_Rect rect = mixSideRect(i);
    if (SDL_PointInRect(&point, &rect)) {
      out.group = kGroupMixSide;
      out.index = i;
      return true;
    }
  }
  return false;
}

bool SdlSurface::isPressed(const ControlEvent& control) const {
  switch (control.group) {
    case kGroupPad:
      return control.index < kNumPads && padPressed_[control.index];
    case kGroupRight:
      return control.index < kNumRightButtons && rightPressed_[control.index];
    case kGroupBottom:
      return control.index < kNumBottomButtons && bottomPressed_[control.index];
    case kGroupShift:
      return shiftPressed_;
    case kGroupMixButton:
      return control.index < kNumMixButtons && mixButtonPressed_[control.index];
    case kGroupMixSide:
      return control.index < kNumMixSideButtons && mixSidePressed_[control.index];
  }
  return false;
}

void SdlSurface::setPressed(const ControlEvent& event) {
  switch (event.group) {
    case kGroupPad:
      if (event.index < kNumPads) padPressed_[event.index] = event.pressed;
      break;
    case kGroupRight:
      if (event.index < kNumRightButtons) rightPressed_[event.index] = event.pressed;
      break;
    case kGroupBottom:
      if (event.index < kNumBottomButtons) bottomPressed_[event.index] = event.pressed;
      break;
    case kGroupShift:
      shiftPressed_ = event.pressed;
      break;
    case kGroupMixButton:
      if (event.index < kNumMixButtons) mixButtonPressed_[event.index] = event.pressed;
      break;
    case kGroupMixSide:
      if (event.index < kNumMixSideButtons) mixSidePressed_[event.index] = event.pressed;
      break;
  }
}

// The mixer: 8 strips of three knobs, A1, A2 and a fader, then the side buttons and the
// mixer's master fader. Laid out like an Akai MIDI Mix.
void SdlSurface::drawMixer() {
  SDL_Renderer* r = renderer_;
  static const char* const kMixSideNames[kNumMixSideButtons] = {"M1", "M2", "M3", "SOLO"};
  setColor(r, kPanel);
  const SDL_Rect panel = {kMixPanelX, kPanelY, kMixPanelRight - kMixPanelX,
                          kMixPanelBottom - kPanelY};
  fillRoundRect(r, panel, kPanelRadius);

  setColor(r, kLabel);
  drawText(r, kMixX, kStatusY, "MIXER", kTextScale);

  char number[3];
  for (uint8_t strip = 0; strip < kNumMixStrips; ++strip) {
    std::snprintf(number, sizeof(number), "%d", strip + 1);
    setColor(r, kLabel);
    drawText(r, kMixX + strip * kStripPitch + (kStripWidth - textWidth(number, kTextScale)) / 2,
             kMixY - kLabelGap - kTextHeight, number, kTextScale);
  }
  for (uint8_t i = 0; i < kNumKnobs; ++i) {
    drawKnob(knobRect(i), knobValues_[i], draggingKnob_ && dragKnob_.index == i);
  }
  for (uint8_t i = 0; i < kNumMixButtons; ++i) {
    const SDL_Rect rect = mixButtonRect(i);
    drawControl(r, rect, frame_.mixButtons[i], mixButtonPressed_[i], kButtonRadius);
    drawPadLabel(r, rect, frame_.mixButtons[i], i < kNumMixStrips ? "MUTE" : "A2");
  }
  for (uint8_t i = 0; i < kNumMixSideButtons; ++i) {
    const SDL_Rect rect = mixSideRect(i);
    drawControl(r, rect, frame_.mixSide[i], mixSidePressed_[i], kButtonRadius);
    drawPadLabel(r, rect, frame_.mixSide[i], kMixSideNames[i]);
  }
  for (uint8_t i = 0; i < kNumMixStrips; ++i) {
    const bool active = draggingFader_ && dragFader_.group == kGroupMixFader &&
                        dragFader_.index == i;
    drawFader(faderRect(kGroupMixFader, i), mixFaderValues_[i], active);
  }
  const SDL_Rect master = faderRect(kGroupMixMaster, 0);
  drawFader(master, mixMasterValue_, draggingFader_ && dragFader_.group == kGroupMixMaster);
  setColor(r, kLabel);
  drawText(r, master.x + (master.w - textWidth("MASTER", kTextScale)) / 2,
           master.y + master.h + kLabelGap, "MASTER", kTextScale);
}

void SdlSurface::drawFader(const SDL_Rect& rect, uint16_t value, bool active) {
  SDL_Renderer* r = renderer_;
  const int centerX = rect.x + rect.w / 2;
  const int top = rect.y + kFaderCapHeight / 2;
  const int travel = rect.h - kFaderCapHeight;

  setColor(r, kHint);  // scale marks at every quarter of the travel
  for (int tick = 0; tick <= 4; ++tick) {
    const int y = top + tick * travel / 4 - 1;
    const SDL_Rect left = {rect.x, y, kFaderTickWidth, 2};
    const SDL_Rect right = {rect.x + rect.w - kFaderTickWidth, y, kFaderTickWidth, 2};
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
  }
  setColor(r, kFaderSlot);
  const SDL_Rect slot = {centerX - kFaderSlotWidth / 2, top, kFaderSlotWidth, travel};
  fillRoundRect(r, slot, kFaderSlotWidth / 2);

  const int capY = rect.y + (kFaderMax - value) * travel / kFaderMax;
  const SDL_Rect cap = {centerX - kFaderCapWidth / 2, capY, kFaderCapWidth, kFaderCapHeight};
  setColor(r, active ? kPressedRing : kFaderCap);
  fillRoundRect(r, cap, kFaderCapRadius);
  setColor(r, kFaderLine);
  const SDL_Rect line = {cap.x + 4, cap.y + kFaderCapHeight / 2 - 1, kFaderCapWidth - 8, 2};
  SDL_RenderFillRect(r, &line);
}

// The open mode is lit, the one under the pointer a shade up from the rest. The hardware has
// no room for these; the simulator does, and it is the one place a mode is named in words.
void SdlSurface::drawModeBar() {
  SDL_Renderer* r = renderer_;
  for (uint8_t i = 0; i < kNumModeButtons; ++i) {
    const SDL_Rect rect = modeRect(i);
    const bool active = kModeOfButton[i] == activeMode_;
    const bool hover = modeAt(mouseX_, mouseY_) == static_cast<int>(i);
    setColor(r, active ? kModeActive : hover ? kModeHover : kModeIdle);
    fillRoundRect(r, rect, kModeButtonRadius);
    setColor(r, active ? kModeActiveText : kLabel);
    drawText(r, rect.x + (rect.w - textWidth(kModeButtons[i].label, kTextScale)) / 2,
             rect.y + (rect.h - kTextHeight) / 2, kModeButtons[i].label, kTextScale);
  }
}

void SdlSurface::draw() {
  SDL_Renderer* r = renderer_;
  setColor(r, kBackground);
  SDL_RenderClear(r);

  drawModeBar();

  setColor(r, kPanel);
  const SDL_Rect panel = {kPanelX, kPanelY, kPanelRight - kPanelX, kPanelBottom - kPanelY};
  fillRoundRect(r, panel, kPanelRadius);

  for (uint8_t i = 0; i < kNumPads; ++i) {
    drawControl(r, padRect(i), frame_.pads[i], padPressed_[i], kPadRadius);
    drawPadLabel(r, padRect(i), frame_.pads[i], padLabels_[i]);
  }

  char label[8];
  const int labelBlockHeight = 2 * kTextHeight + kLabelLineGap;
  for (uint8_t i = 0; i < kNumRightButtons; ++i) {
    const SDL_Rect rect = rightRect(i);
    drawControl(r, rect, frame_.right[i], rightPressed_[i], kButtonRadius);
    const int x = rect.x + rect.w + kLabelGap;
    const int y = rect.y + (rect.h - labelBlockHeight) / 2;
    std::snprintf(label, sizeof(label), "R%d", i + 1);
    setColor(r, kHint);
    drawText(r, x, y, label, kTextScale);
    setColor(r, labelsShifted_ ? kStatus : kLabel);
    drawText(r, x, y + kTextHeight + kLabelLineGap, rightLabels_[i], kTextScale);
  }
  for (uint8_t i = 0; i < kNumBottomButtons; ++i) {
    const SDL_Rect rect = bottomRect(i);
    drawControl(r, rect, frame_.bottom[i], bottomPressed_[i], kButtonRadius);
    std::snprintf(label, sizeof(label), "B%d", i + 1);
    const char* text = bottomLabels_[i] ? bottomLabels_[i] : label;
    // Brighter whenever the core relabels the button, e.g. as a page while R1 is held.
    setColor(r, labelsShifted_ || bottomLabels_[i] ? kStatus : kLabel);
    drawText(r, rect.x + (rect.w - textWidth(text, kTextScale)) / 2,
             rect.y + rect.h + kLabelGap, text, kTextScale);
  }

  const SDL_Rect shift = shiftRect();
  drawControl(r, shift, frame_.shift, shiftPressed_, kButtonRadius);
  setColor(r, kLabel);
  drawText(r, shift.x + (shift.w - textWidth("SHIFT", kTextScale)) / 2,
           shift.y + shift.h + kLabelGap, "SHIFT", kTextScale);

  for (uint8_t i = 0; i < kNumTrackFaders; ++i) {
    const bool active = draggingFader_ && dragFader_.group == kGroupFader && dragFader_.index == i;
    drawFader(faderRect(kGroupFader, i), faderValues_[i], active);
  }
  const SDL_Rect master = faderRect(kGroupMasterFader, 0);
  drawFader(master, masterFaderValue_, draggingFader_ && dragFader_.group == kGroupMasterFader);
  setColor(r, kLabel);
  drawText(r, master.x + (master.w - textWidth("MASTER", kTextScale)) / 2,
           master.y + master.h + kLabelGap, "MASTER", kTextScale);

  drawMixer();

  setColor(r, kStatus);
  drawText(r, kGridX, kStatusY, status_, kTextScale);

  setColor(r, kHint);
  drawText(r, kGridX, kFooterY,
           "CLICK: PRESS  R-CLICK: LATCH  DRAG OR WHEEL: FADERS AND KNOBS (CTRL: FINE)",
           kTextScale);
  drawText(r, kGridX, kFooterY + kTextHeight + kFooterLineGap,
           "F1-F8: R1-R8  SPACE: PLAY  1-8: B1-B8  SHIFT: SHIFT  ESC: QUIT", kTextScale);
}

bool SdlSurface::saveScreenshot(const char* path) {
  int w = 0;
  int h = 0;
  if (SDL_GetRendererOutputSize(renderer_, &w, &h) != 0) return false;
  SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!surface) return false;
  const bool ok = SDL_RenderReadPixels(renderer_, NULL, SDL_PIXELFORMAT_ARGB8888, surface->pixels,
                                       surface->pitch) == 0 &&
                  SDL_SaveBMP(surface, path) == 0;
  SDL_FreeSurface(surface);
  return ok;
}

void SdlSurface::present() {
  SDL_RenderPresent(renderer_);
  if (!vsync_) SDL_Delay(8);
}

}  // namespace gx
