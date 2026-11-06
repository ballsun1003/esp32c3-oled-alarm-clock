#pragma once
#include "oled_motion.h"

namespace oled {
inline float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
class UiAnimation {
  uint8_t last[1024] = {}, outgoing[1024] = {}, incoming[1024] = {};
  int view = -1, depth = 0, direction = 1, displayedShift = 0;
  uint32_t pageStart = 0, focusStart = 0;
  bool sliding = false, hasFocus = false;
  bool activation = false, pulse = false, explicitPulse = false;
  Rect pulseRect = {}, renderedFocus = {}, displayedFocus = {};
  Rect focusFrom = {}, focusTo = {};

 public:
  void activate() { activation = true; }
  void activate(Rect r) {
    activation = true;
    explicitPulse = true;
    pulseRect = r;
    pulseRect.x += displayedShift;
  }
  void cancelActivation() { activation = explicitPulse = false; }
  void begin(int nextView, int nextDepth, uint32_t now, bool animate) {
    if (nextView == view) return;
    if ((animate || activation) && view >= 0) {
      memcpy(outgoing, last, sizeof(last));
      pageStart = now;
      sliding = animate;
      direction = nextDepth < depth ? -1 : 1;
    } else
      sliding = false;
    pulse = view >= 0 && activation && (explicitPulse || hasFocus);
    if (!explicitPulse) pulseRect = displayedFocus;
    activation = explicitPulse = false;
    view = nextView;
    depth = nextDepth;
    hasFocus = false;
  }
  Rect focus(Rect target, uint32_t now, bool editing = false) {
    // An editing selection must surround its actual field immediately, even if
    // OK was pressed before the selection finished moving across other text.
    if (!hasFocus || editing) {
      focusFrom = focusTo = target;
      focusStart = now;
      hasFocus = true;
    }
    if (target.x != focusTo.x || target.y != focusTo.y || target.w != focusTo.w ||
        target.h != focusTo.h) {
      focusFrom = current(now);
      focusTo = target;
      focusStart = now;
    }
    renderedFocus = current(now);
    return renderedFocus;
  }
  Rect current(uint32_t now) const {
    float t = easeOut(limit((now - focusStart) / 120.f, 0, 1));
    int left = pixel(focusFrom.x + (focusTo.x - focusFrom.x) * t);
    int top = pixel(focusFrom.y + (focusTo.y - focusFrom.y) * t);
    int right =
        pixel(focusFrom.x + focusFrom.w + (focusTo.x + focusTo.w - focusFrom.x - focusFrom.w) * t);
    int bottom =
        pixel(focusFrom.y + focusFrom.h + (focusTo.y + focusTo.h - focusFrom.y - focusFrom.h) * t);
    return {left, top, right - left, bottom - top};
  }
  void compose(uint8_t* buffer, uint32_t now) {
    int incomingShift = 0;
    if (sliding) {
      float t = limit((now - pageStart) / 180.f, 0, 1);
      int shift = pixel(128 * easeOut(t));
      if (shift == 0) shift = 1;  // First pulse frame already moves the outgoing page.
      incomingShift = direction * (128 - shift);
      memcpy(incoming, buffer, 1024);
      for (int page = 0; page < 8; page++)
        for (int x = 0; x < 128; x++) {
          int oldX = x + direction * shift, newX = x - direction * (128 - shift);
          buffer[page * 128 + x] = newX >= 0 && newX < 128   ? incoming[page * 128 + newX]
                                   : oldX >= 0 && oldX < 128 ? outgoing[page * 128 + oldX]
                                                             : 0;
          if (pulse && oldX >= pulseRect.x + 1 && oldX < pulseRect.x + pulseRect.w - 1 &&
              oldX >= 0 && oldX < 128 && !(newX >= 0 && newX < 128)) {
            for (int bit = 0; bit < 8; bit++) {
              int y = page * 8 + bit;
              if (y >= pulseRect.y + 1 && y < pulseRect.y + pulseRect.h - 1)
                buffer[page * 128 + x] ^= 1U << bit;
            }
          }
        }
      if (t >= 1) sliding = false;
    } else if (pulse) {
      // A submitted keyboard command may open a loading overlay in place.
      // Present its acknowledgement once; the operation itself is not delayed.
      memcpy(buffer, outgoing, 1024);
      for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
          if (x > pulseRect.x && x < pulseRect.x + pulseRect.w - 1 && y > pulseRect.y &&
              y < pulseRect.y + pulseRect.h - 1)
            buffer[(y >> 3) * 128 + x] ^= 1U << (y & 7);
    }
    displayedFocus = renderedFocus;
    displayedFocus.x += incomingShift;
    displayedShift = incomingShift;
    memcpy(last, buffer, 1024);
    pulse = false;
  }
};
}  // namespace oled
