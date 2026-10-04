#include "display/DisplayWindow.h"

#include "engine/Sequencer.h"  // ClockSource: the names the top band prints

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gx {

namespace {

// The panel this was drawn for. Everything is placed against these, so another size would
// need the bands re-proportioned rather than merely scaled.
const int kWidth = 800;
const int kHeight = 480;
const int kTopHeight = 78;
const int kBottomHeight = 84;
const int kEdge = 18;  // the side gutter every band shares

// The type scale, small enough to keep to. A screen glanced at from arm's length wants few
// sizes and big ones: anything subtler is unreadable in the room this lives in.
enum FontSize { kHuge = 0, kLarge, kBody, kSmall, kNumFontSizes };
const int kFontPixels[kNumFontSizes] = {52, 26, 18, 13};

// Where a monospaced font usually is. The config can name one instead.
const char* const kFontPaths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
};

const Rgb kBg = {10, 11, 13};
const Rgb kBand = {16, 18, 22};
const Rgb kRule = {33, 37, 44};
const Rgb kInk = {233, 236, 241};
const Rgb kDim = {121, 130, 142};
const Rgb kFaint = {70, 78, 88};
const Rgb kGreen = {54, 214, 95};
const Rgb kRed = {255, 43, 43};

SDL_Color toSdl(Rgb color, uint8_t alpha) {
  SDL_Color out;
  out.r = color.r;
  out.g = color.g;
  out.b = color.b;
  out.a = alpha;
  return out;
}

// "3.08": projects and presets are named page.pad, counting from 1.
std::string slotText(uint16_t slot) {
  char out[16];
  std::snprintf(out, sizeof(out), "%u.%02u", slot / 64u + 1u, slot % 64u + 1u);
  return out;
}

std::string number(uint16_t value) {
  char out[12];
  std::snprintf(out, sizeof(out), "%u", value);
  return out;
}

const char* clockName(uint8_t source) {
  switch (source) {
    case kClockInternal: return "INT";
    case kClockExternal: return "EXT";
    default: return "AUTO";
  }
}

}  // namespace

DisplayWindow::DisplayWindow() : window_(NULL), renderer_(NULL), naming_(NULL) {
  for (int i = 0; i < kNumFontSizes; ++i) fonts_[i] = NULL;
}

DisplayWindow::~DisplayWindow() {
  for (int i = 0; i < kNumFontSizes; ++i) {
    if (fonts_[i]) TTF_CloseFont(fonts_[i]);
  }
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  if (TTF_WasInit()) TTF_Quit();
}

bool DisplayWindow::open(const std::string& fontPath) {
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "display: no video: %s\n", SDL_GetError());
    return false;
  }
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "display: no font engine: %s\n", TTF_GetError());
    return false;
  }

  // The font is looked for once and opened at every size the panel uses, because TTF_Font is
  // a size as well as a face.
  std::string path = fontPath;
  if (path.empty()) {
    for (size_t i = 0; i < sizeof(kFontPaths) / sizeof(kFontPaths[0]); ++i) {
      if (std::FILE* probe = std::fopen(kFontPaths[i], "rb")) {
        std::fclose(probe);
        path = kFontPaths[i];
        break;
      }
    }
  }
  if (path.empty()) {
    std::fprintf(stderr, "display: no monospaced font found - name one with 'displayfont' in "
                         "controls.conf\n");
    return false;
  }
  for (int i = 0; i < kNumFontSizes; ++i) {
    fonts_[i] = TTF_OpenFont(path.c_str(), kFontPixels[i]);
    if (!fonts_[i]) {
      std::fprintf(stderr, "display: cannot use '%s': %s\n", path.c_str(), TTF_GetError());
      return false;
    }
  }

  // A desktop means someone is working at this machine, so the panel is a window among their
  // others. With neither X nor Wayland there is nothing to be a window on - the Pi is driving
  // the screen itself - and it takes the whole of it.
  const bool desktop = std::getenv("DISPLAY") != NULL || std::getenv("WAYLAND_DISPLAY") != NULL;
  window_ = SDL_CreateWindow("GroovixBox", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             kWidth, kHeight, desktop ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window_) {
    std::fprintf(stderr, "display: no window: %s\n", SDL_GetError());
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer_) {
    std::fprintf(stderr, "display: no renderer: %s\n", SDL_GetError());
    return false;
  }
  // A fullscreen panel may not be 800x480; let SDL letterbox rather than re-laying out.
  SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
  // The pointer is in the way on a panel nobody can touch, but on a desktop it is theirs.
  if (!desktop) SDL_ShowCursor(SDL_DISABLE);
  std::printf("display: 800x480 %s on %s, font %s\n", desktop ? "window" : "fullscreen",
              SDL_GetCurrentVideoDriver(), path.c_str());
  return true;
}

void DisplayWindow::fill(int x, int y, int w, int h, Rgb color, uint8_t alpha) {
  SDL_SetRenderDrawBlendMode(renderer_, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, alpha);
  SDL_Rect rect = {x, y, w, h};
  SDL_RenderFillRect(renderer_, &rect);
}

int DisplayWindow::draw(const char* text, int x, int y, uint8_t size, Rgb color, uint8_t alpha) {
  if (!text || !*text || size >= kNumFontSizes) return 0;
  SDL_Surface* surface = TTF_RenderUTF8_Blended(fonts_[size], text, toSdl(color, alpha));
  if (!surface) return 0;
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
  const int w = surface->w, h = surface->h;
  SDL_FreeSurface(surface);
  if (!texture) return 0;
  if (alpha != 255) {
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  }
  SDL_Rect to = {x, y, w, h};
  SDL_RenderCopy(renderer_, texture, NULL, &to);
  SDL_DestroyTexture(texture);
  return w;
}

int DisplayWindow::draw(const std::string& text, int x, int y, uint8_t size, Rgb color,
                        uint8_t alpha) {
  return draw(text.c_str(), x, y, size, color, alpha);
}

int DisplayWindow::width(const char* text, uint8_t size) const {
  if (!text || !*text || size >= kNumFontSizes) return 0;
  int w = 0, h = 0;
  TTF_SizeUTF8(fonts_[size], text, &w, &h);
  return w;
}

void DisplayWindow::drawClipped(const std::string& text, int x, int y, int maxWidth, uint8_t size,
                                Rgb color, uint8_t alpha) {
  if (text.empty()) return;
  if (width(text.c_str(), size) <= maxWidth) {
    draw(text, x, y, size, color, alpha);
    return;
  }
  // A voice name can be longer than its column; cut it rather than letting it run into the
  // pattern number beside it.
  std::string cut = text;
  while (!cut.empty() && width((cut + "...").c_str(), size) > maxWidth) {
    cut.erase(cut.size() - 1);
  }
  draw(cut + "...", x, y, size, color, alpha);
}

void DisplayWindow::drawTransport(const DisplayFrame& frame) {
  fill(0, 0, kWidth, kTopHeight, kBand);
  fill(0, kTopHeight - 1, kWidth, 1, kRule);

  const std::string bpm = number(frame.bpm);
  const int bpmWidth = draw(bpm, kEdge, 12, kHuge, kInk);
  draw("BPM", kEdge + bpmWidth + 8, 48, kSmall, kDim);

  const int clockX = kEdge + bpmWidth + 60;
  draw("CLOCK", clockX, 20, kSmall, kFaint);
  // Green while an outside clock is really driving, so a master that has stopped shows as a
  // dark dot rather than as silence nobody can account for.
  const Rgb dot = frame.followingExternal ? kGreen : kFaint;
  fill(clockX, 43, 9, 9, dot);
  draw(clockName(frame.clockSource), clockX + 16, 38, kBody, kInk);

  const char* const play = frame.playing ? "PLAY" : "STOP";
  const int recWidth = width("REC", kBody) + 40;
  const int playWidth = width(play, kBody) + 40;
  const int recX = kWidth - kEdge - recWidth;
  const int playX = recX - 12 - playWidth;
  if (frame.playing) fill(playX, 22, playWidth, 34, kGreen, 40);
  draw(play, playX + 20, 29, kBody, frame.playing ? kGreen : kFaint);
  if (frame.recording) fill(recX, 22, recWidth, 34, kRed, 40);
  draw("REC", recX + 20, 29, kBody, frame.recording ? kRed : kFaint);
}

void DisplayWindow::drawTracks(const DisplayFrame& frame) {
  const int top = kTopHeight;
  const int height = kHeight - kTopHeight - kBottomHeight;
  const int rows = frame.numTracks > 0 ? frame.numTracks : 1;
  const int rowHeight = height / rows;
  for (uint8_t i = 0; i < frame.numTracks; ++i) {
    const DisplayTrack& track = frame.tracks[i];
    const int y = top + rowHeight * i;
    const uint8_t alpha = track.muted ? 90 : 255;
    fill(0, y + rowHeight - 1, kWidth, 1, {23, 27, 33});
    fill(0, y, 5, rowHeight - 1, track.color, track.muted ? 56 : 255);

    int x = 16;
    draw(number(track.number), x, y + rowHeight / 2 - 10, kBody, track.color, alpha);
    x += 30;

    char route[16];
    if (track.port == kNoDisplayPort) {
      std::snprintf(route, sizeof(route), "I%u", track.instrument + 1u);
    } else {
      std::snprintf(route, sizeof(route), "P%u ch%u", track.port + 1u, track.channel + 1u);
    }
    draw(route, x, y + rowHeight / 2 - 8, kSmall, kDim, alpha);
    x += 84;

    const std::string voice = naming_ ? naming_->voiceName(track.port, track.preset) : std::string();
    drawClipped(voice.empty() ? slotText(track.preset) : voice, x, y + rowHeight / 2 - 11,
                kWidth - x - 180, kBody, kInk, alpha);

    char pattern[16];
    std::snprintf(pattern, sizeof(pattern), "PAT %u", track.pattern);
    draw(pattern, kWidth - 172, y + rowHeight / 2 - 8, kSmall, kDim, alpha);

    // Four bars, one per step up to the playhead: what this track has just been playing.
    for (uint8_t bar = 0; bar < 4; ++bar) {
      const bool hit = (track.activity & (1u << bar)) != 0;
      const int barHeight = hit ? 14 : 3;
      fill(kWidth - 108 + bar * 11, y + rowHeight / 2 + 7 - barHeight, 8, barHeight,
           track.color, hit ? 220 : 60);
    }

    if (track.soloed) draw("SOLO", kWidth - 54, y + rowHeight / 2 - 7, kSmall, kInk);
    else if (track.muted) draw("MUTE", kWidth - 54, y + rowHeight / 2 - 7, kSmall, kFaint);
  }
}

void DisplayWindow::drawLegend(const DisplayFrame& frame) {
  const int top = kTopHeight;
  if (!frame.legend) return;
  const ModeLegend& legend = *frame.legend;

  // The mode's own key, so the screen says how you got here and how to get back.
  const int keyWidth = width(legend.key, kSmall) + 16;
  fill(kEdge, top + 16, keyWidth, 26, kInk);
  draw(legend.key, kEdge + 8, top + 20, kSmall, kBg);
  draw(legend.name, kEdge + keyWidth + 14, top + 12, kLarge, kInk);

  // The right of the header says what is being worked on: the track, its pattern, and which
  // 32 steps the grid is showing - the last being the thing the pads themselves cannot say.
  std::string context;
  if (frame.contextTrack) context = "TRACK " + number(frame.contextTrack);
  if (frame.contextPattern) context += "  PAT " + number(frame.contextPattern);
  if (frame.contextFirstStep) {
    context += "  STEPS " + number(frame.contextFirstStep) + "-" + number(frame.contextLastStep);
  }
  if (!context.empty()) {
    draw(context, kWidth - kEdge - width(context.c_str(), kSmall), top + 22, kSmall, kDim);
  }

  // The grid, eight rows of eight pads: it shows the shape before a word is read.
  const int gridX = kEdge, gridY = top + 56, gridW = 152, gridH = 212;
  fill(gridX, gridY, gridW, gridH, {7, 8, 10});
  for (int row = 0; row < kGridRows; ++row) {
    const int rowY = gridY + 7 + row * ((gridH - 14) / kGridRows);
    const int padH = (gridH - 14) / kGridRows - 4;
    for (int col = 0; col < kGridCols; ++col) {
      const int padW = (gridW - 14) / kGridCols - 4;
      fill(gridX + 7 + col * ((gridW - 14) / kGridCols), rowY, padW, padH, legend.rowColor[row]);
    }
  }

  // One line per band, beside the grid. The label column is as wide as the widest label
  // actually there, measured - a fixed guess is how "SHIFT step" ended up touching its text.
  const int textX = gridX + gridW + 22;
  const int labelX = textX + 20;
  int labelWidth = 0;
  for (uint8_t i = 0; i < legend.numBands; ++i) {
    if (!legend.bands[i].label) continue;
    const int w = width(legend.bands[i].label, kBody);
    if (w > labelWidth) labelWidth = w;
  }
  const int bandTextX = labelX + labelWidth + 18;
  const int bandTextWidth = kWidth - bandTextX - kEdge;
  int y = gridY + 4;
  for (uint8_t i = 0; i < legend.numBands; ++i) {
    const DisplayBand& band = legend.bands[i];
    if (!band.label) continue;
    fill(textX, y + 6, 11, 11, band.color);
    draw(band.label, labelX, y, kBody, kInk);
    drawClipped(band.text, bandTextX, y, bandTextWidth, kBody, kDim);
    y += 30;
  }

  // The modifiers, smaller, under the bands.
  int modX = textX + 20;
  for (uint8_t i = 0; i < legend.numMods && legend.mods[i]; ++i) {
    const int w = width(legend.mods[i], kSmall);
    if (modX + w > kWidth - kEdge) {
      modX = textX + 20;
      y += 20;
    }
    draw(legend.mods[i], modX, y + 6, kSmall, kFaint);
    modX += w + 22;
  }
}

void DisplayWindow::drawValues(const DisplayFrame& frame) {
  const int top = kHeight - kBottomHeight;
  fill(0, top, kWidth, kBottomHeight, kBand);
  fill(0, top, kWidth, 1, kRule);

  int x = kEdge;
  for (uint8_t i = 0; i < frame.numValues; ++i) {
    const DisplayValue& value = frame.values[i];
    if (i > 0) {
      fill(x - 13, top + 16, 1, kBottomHeight - 32, kRule);
    }
    draw(value.key, x, top + 18, kSmall, kFaint);
    int used = value.text ? draw(value.text, x, top + 38, kLarge, kInk)
                          : draw(number(value.number), x, top + 38, kLarge, kInk);
    if (value.suffix) used += draw(value.suffix, x + used + 6, top + 46, kBody, kDim) + 6;
    const int keyWidth = width(value.key, kSmall);
    x += (used > keyWidth ? used : keyWidth) + 40;
  }

  const std::string project = slotText(frame.project);
  draw("PROJECT", kWidth - kEdge - width("PROJECT", kSmall), top + 18, kSmall, kFaint);
  draw(project, kWidth - kEdge - width(project.c_str(), kBody), top + 42, kBody, kDim);
}

void DisplayWindow::show(const DisplayFrame& frame) {
  if (!renderer_) return;
  fill(0, 0, kWidth, kHeight, kBg);
  drawTransport(frame);
  // Only the middle changes: running, what every track is doing; stopped, what the rows are
  // for. The bands above and below stay put so the screen never rearranges itself.
  if (frame.showTracks) {
    drawTracks(frame);
  } else {
    drawLegend(frame);
  }
  drawValues(frame);
  SDL_RenderPresent(renderer_);
}

}  // namespace gx
