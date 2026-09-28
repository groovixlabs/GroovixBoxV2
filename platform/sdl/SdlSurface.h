#pragma once

#include <SDL.h>

#include "ui/ControlSurface.h"

namespace gx {

// ControlSurface that draws the hardware in an SDL window. Mouse and keyboard become
// control presses: click = press (held while the button is down), right-click = latch a
// control down until it is right-clicked again (so combinations needing several pads at
// once can be played with one mouse), F1-F8 = R1-R8, Space = R8, 1-8 = B1-B8,
// Shift = Shift. Drag a fader (clicking along its slot jumps the cap there) or roll the mouse
// wheel over it to move it, with Ctrl for fine steps. Esc or closing the window requests quit.
class SdlSurface : public ControlSurface {
 public:
  // Layout in logical window pixels.
  enum {
    kMargin = 28,
    kPanelPadding = 14,
    kPadSize = 64,
    kPadGap = 10,
    kPadRadius = 8,
    kButtonSize = 40,  // R and B buttons are smaller than the pads
    kButtonGap = 22,   // distance from the grid to the R/B buttons
    kButtonRadius = 6,
    kTextScale = 2,
    kTextHeight = 5 * kTextScale,
    kLabelGap = 8,
    kLabelChars = 9,  // longest right-button label, "DUPLICATE"
    kLabelWidth = kLabelChars * 4 * kTextScale - kTextScale,

    kPitch = kPadSize + kPadGap,
    kGridX = kMargin,
    kGridY = kMargin + 2 * kTextHeight,
    kGridWidth = kGridCols * kPitch - kPadGap,
    kGridHeight = kGridRows * kPitch - kPadGap,
    kRightX = kGridX + kGridWidth + kButtonGap,
    kBottomY = kGridY + kGridHeight + kButtonGap,

    kPanelX = kGridX - kPanelPadding,
    kPanelY = kGridY - kPanelPadding,
    kPanelRight = kRightX + kButtonSize + kLabelGap + kLabelWidth + kPanelPadding,
    kFaderGap = 18,  // from the B / Shift labels to the faders
    kFaderTravel = 112,
    kFaderCapWidth = 36,
    kFaderCapHeight = 16,
    kFaderSlotWidth = 6,
    kFaderY = kBottomY + kButtonSize + kLabelGap + kTextHeight + kFaderGap,
    kFaderHeight = kFaderTravel + kFaderCapHeight,
    kFooterLineGap = 6,

    kPanelBottom = kFaderY + kFaderHeight + kLabelGap + kTextHeight + kPanelPadding,
    kFooterY = kPanelBottom + 16,

    // The mixer, to the right: 8 strips of three knobs, A1, A2 and a fader, then a column
    // with a button beside each knob row, one beside the A1 row, and the master fader.
    kMixGap = 26,  // between the two panels
    kKnobSize = 52,
    kKnobPitch = 80,
    kMixButtonWidth = 46,
    kMixButtonHeight = 30,
    kMixButtonPitch = 42,
    kStripPitch = 68,
    kStripWidth = 56,
    kMixFaderTravel = 150,
    kMixFaderHeight = kMixFaderTravel + kFaderCapHeight,
    kKnobDragRange = 160,  // pixels of drag from the bottom of a knob's range to the top

    kMixPanelX = kPanelRight + kMixGap,
    kMixX = kMixPanelX + kPanelPadding,
    kMixY = kGridY,  // the top knob row starts level with the pad grid
    kMixButtonsY = kMixY + kKnobRows * kKnobPitch,
    kMixFaderY = kMixButtonsY + kNumMixButtonRows * kMixButtonPitch + 8,
    kMixSideX = kMixX + kNumMixStrips * kStripPitch,
    kMixMasterY = kMixButtonsY + kMixButtonPitch,
    kMixMasterHeight = kMixFaderY + kMixFaderHeight - kMixMasterY,
    kMixPanelRight = kMixSideX + kStripWidth + kPanelPadding,
    kMixPanelBottom = kMixFaderY + kMixFaderHeight + kLabelGap + kTextHeight + kPanelPadding,

    kWindowWidth = kMixPanelRight + kPanelX,
    kWindowHeight = kFooterY + 2 * kTextHeight + kFooterLineGap + kPanelX,
  };

  SdlSurface();
  ~SdlSurface();
  SdlSurface(const SdlSurface&) = delete;
  SdlSurface& operator=(const SdlSurface&) = delete;

  // Opens the window. Prints the reason and returns false on failure.
  bool init(const char* title);

  bool pollEvent(ControlEvent& event) override;
  void show(const LedFrame& frame) override;

  bool quitRequested() const { return quitRequested_; }
  // Simulator-only text drawn above the surface.
  void setStatus(const char* text);
  // Name printed under a right button's R1..R8 label. text must outlive the surface.
  void setRightLabel(uint8_t index, const char* text);
  // Text under a bottom button; NULL shows B1..B8. text must outlive the surface.
  void setBottomLabel(uint8_t index, const char* text);
  // Text printed on a pad, lines split by '\n'; NULL for none. text must outlive the surface.
  void setPadLabel(uint8_t index, const char* text);
  // Draws the labels brighter, e.g. while they show the Shift functions.
  void setLabelsShifted(bool shifted) { labelsShifted_ = shifted; }
  // Moves a fader's cap without reporting it, e.g. to follow a hardware fader.
  void setFaderPosition(uint8_t group, uint8_t index, uint16_t value);

  // Draws the last frame passed to show(); optionally saveScreenshot(), then present().
  void draw();
  bool saveScreenshot(const char* path);
  void present();

 private:
  bool hitTest(int x, int y, ControlEvent& out) const;
  bool isPressed(const ControlEvent& control) const;
  void setPressed(const ControlEvent& event);
  static SDL_Rect padRect(uint8_t index);
  static SDL_Rect rightRect(uint8_t index);
  static SDL_Rect bottomRect(uint8_t index);
  static SDL_Rect shiftRect();
  // Mixer: knobs and A1/A2 buttons are indexed row by row, side buttons top to bottom.
  static SDL_Rect knobRect(uint8_t index);
  static SDL_Rect mixButtonRect(uint8_t index);
  static SDL_Rect mixSideRect(uint8_t index);
  // A track fader (kGroupFader) or the master fader, including the cap's full travel.
  static SDL_Rect faderRect(uint8_t group, uint8_t index);
  bool faderAt(int x, int y, ControlEvent& out) const;
  bool knobAt(int x, int y, ControlEvent& out) const;
  // Fills out with the control reported at index i of the start-up scan; false past the end.
  bool syncControl(uint8_t i, ControlEvent& out) const;
  void drawMixer();
  uint16_t& faderValue(const ControlEvent& fader);
  // Sets a fader, clamped to 0..kFaderMax. Returns true, with event filled in, if it moved.
  bool moveFader(const ControlEvent& fader, int value, ControlEvent& event);
  void drawFader(const SDL_Rect& rect, uint16_t value, bool active);
  void drawKnob(const SDL_Rect& rect, uint16_t value, bool active);

  SDL_Window* window_;
  SDL_Renderer* renderer_;
  bool videoInitialized_;
  bool vsync_;
  bool quitRequested_;
  bool mouseHeld_;
  ControlEvent mouseControl_;
  LedFrame frame_;
  char status_[128];
  const char* rightLabels_[kNumRightButtons];
  const char* bottomLabels_[kNumBottomButtons];  // NULL: B1..B8
  const char* padLabels_[kNumPads];              // NULL: none
  bool labelsShifted_;
  bool padPressed_[kNumPads];
  bool rightPressed_[kNumRightButtons];
  bool bottomPressed_[kNumBottomButtons];
  bool shiftPressed_;
  uint16_t faderValues_[kNumTrackFaders];
  uint16_t masterFaderValue_;
  uint16_t knobValues_[kNumKnobs];
  uint16_t mixFaderValues_[kNumMixStrips];
  uint16_t mixMasterValue_;
  bool mixButtonPressed_[kNumMixButtons];
  bool mixSidePressed_[kNumMixSideButtons];
  uint8_t faderSync_;  // next control to report at start; past the last when done
  bool draggingFader_;
  ControlEvent dragFader_;
  bool draggingKnob_;      // knobs turn by how far the pointer moves, not where it is
  ControlEvent dragKnob_;
  int dragStartY_;
  int dragStartValue_;
  int mouseX_;  // last pointer position, for the mouse wheel
  int mouseY_;
};

}  // namespace gx
