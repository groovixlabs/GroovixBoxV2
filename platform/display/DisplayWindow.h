#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>

#include "ui/DisplayFrame.h"

namespace gx {

// The screen beside the instrument: an 800x480 panel on a Raspberry Pi, drawn with SDL.
//
// It is a DisplaySurface and nothing else - it is handed a finished DisplayFrame and turns it
// into pixels. Everything it knows about the sequencer arrives in that struct, so the UI never
// learns what a font is and this file never reaches into the engine.
//
// On a Pi with no X server, SDL's KMSDRM backend draws straight to the display, which is why
// this needs no desktop. Set SDL_VIDEODRIVER if a particular backend is wanted.
class DisplayWindow : public DisplaySurface {
 public:
  DisplayWindow();
  ~DisplayWindow();
  DisplayWindow(const DisplayWindow&) = delete;
  DisplayWindow& operator=(const DisplayWindow&) = delete;

  // Opens the panel. `fontPath` may be empty, which looks for a monospaced font in the usual
  // places. False, with why on stderr, when there is no screen or no font - the instrument
  // then runs exactly as it does without one.
  //
  // It takes the whole screen only when there is no desktop to put a window on: the built rig
  // is a Pi driving the panel directly, where a window would be a window over nothing, while
  // a Linux box with X or Wayland running is someone working, who wants the rest of their
  // screen back.
  bool open(const std::string& fontPath);
  bool isOpen() const { return renderer_ != NULL; }

  // DisplaySurface: draws the frame. Cheap when nothing has changed - the panel is redrawn
  // only when the frame differs from the one on screen, so a stopped instrument costs nothing.
  void show(const DisplayFrame& frame) override;

  // How the platform names a voice and formats what core left as numbers. Set before show();
  // without it a track's voice column is blank and everything else still draws.
  class Naming {
   public:
    virtual ~Naming() {}
    // The voice a slot names on that port, or "" when the device's list has no name for it.
    virtual std::string voiceName(uint8_t port, uint16_t slot) const = 0;
  };
  void setNaming(const Naming* naming) { naming_ = naming; }

 private:
  struct Text;  // a cached rendered string

  void drawTransport(const DisplayFrame& frame);
  void drawTracks(const DisplayFrame& frame);
  void drawLegend(const DisplayFrame& frame);
  void drawValues(const DisplayFrame& frame);

  void fill(int x, int y, int w, int h, Rgb color, uint8_t alpha = 255);
  // Draws text and returns how wide it was, so a caller can put something after it.
  int draw(const char* text, int x, int y, uint8_t size, Rgb color, uint8_t alpha = 255);
  int draw(const std::string& text, int x, int y, uint8_t size, Rgb color, uint8_t alpha = 255);
  int width(const char* text, uint8_t size) const;
  // Draws text clipped to `maxWidth`, ending in an ellipsis when it would not fit.
  void drawClipped(const std::string& text, int x, int y, int maxWidth, uint8_t size, Rgb color,
                   uint8_t alpha = 255);

  SDL_Window* window_;
  SDL_Renderer* renderer_;
  TTF_Font* fonts_[4];  // the type scale: see kFontSizes
  const Naming* naming_;
};

}  // namespace gx
