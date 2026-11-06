#pragma once
#include <stdio.h>

#include "oled_ui.h"

namespace oled {
// SSD1306/U8g2 R0 full buffer: 16 horizontal 8px tiles in each of eight pages.
// Reuse the comparison buffer; only the changed span in each page crosses I2C.
template <class Canvas>
void presentChanged(Canvas& c, uint8_t* previous, bool& valid) {
  uint8_t* frame = c.getBufferPtr();
  if (!valid) {
    c.sendBuffer();
    memcpy(previous, frame, 1024);
    valid = true;
    return;
  }
  for (int page = 0; page < 8; page++) {
    int first = 16, last = -1;
    for (int tile = 0; tile < 16; tile++)
      if (memcmp(frame + page * 128 + tile * 8, previous + page * 128 + tile * 8, 8) != 0) {
        if (first == 16) first = tile;
        last = tile;
      }
    if (last >= first) {
      c.updateDisplayArea(first, page, last - first + 1, 1);
      memcpy(previous + page * 128 + first * 8, frame + page * 128 + first * 8,
             (last - first + 1) * 8);
    }
  }
}
// Fixed digit slots keep unchanged places, punctuation and units stationary.
// Callers reserve leading spaces for the widest value of each numeric field.
class RollingText {
  int key = -1, direction = 1;
  uint32_t started = 0;
  char previous[32] = {}, latest[32] = {};
  static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
  static bool numeric(const char* text) {
    while (*text == ' ') ++text;
    return digit(*text);
  }

 public:
  void reset() { key = -1; }
  template <class Canvas>
  void draw(Canvas& c, int field, const char* text, int x, int baseline, Rect clip, int movement,
            uint32_t now, bool animate) {
    if (!animate || key != field) {
      key = field;
      snprintf(latest, sizeof(latest), "%s", text);
      previous[0] = 0;
    } else if (strcmp(latest, text) != 0) {
      memcpy(previous, latest, sizeof(previous));
      snprintf(latest, sizeof(latest), "%s", text);
      direction = movement < 0 ? -1 : 1;
      started = now;
    }
    uint32_t elapsed = now - started;
    bool turn = previous[0] && elapsed < 40 && numeric(previous) && numeric(text) &&
                strlen(previous) == strlen(text);
    if (!numeric(text)) {
      c.setClipWindow(clip.x, clip.y, clip.x + clip.w, clip.y + clip.h);
      c.drawStr(x, baseline, text);
    } else {
      int cell = c.getStrWidth("00") - c.getStrWidth("0");
      for (int i = 0; text[i]; ++i) {
        int left = x + i * cell, x0 = left > clip.x ? left : clip.x;
        int x1 = left + cell < clip.x + clip.w ? left + cell : clip.x + clip.w;
        if (x1 <= x0) continue;
        c.setClipWindow(x0, clip.y, x1, clip.y + clip.h);
        char next[] = {text[i], 0}, old[] = {turn ? previous[i] : text[i], 0};
        bool changed = turn && old[0] != next[0] && (digit(old[0]) || old[0] == ' ') &&
                       (digit(next[0]) || next[0] == ' ');
        if (changed) {
          int shift = clip.h * (int(elapsed / 20) + 1) / 3;
          c.drawStr(left, baseline - direction * shift, old);
          c.drawStr(left, baseline + direction * (clip.h - shift), next);
        } else
          c.drawStr(left, baseline, next);
      }
    }
    c.setMaxClipWindow();
  }
};
class CarouselSlide {
  float from = 0;
  uint32_t started = 0;

 public:
  float offset(uint32_t now) const {
    return from * (1 - easeOut(limit((now - started) / 80.f, 0, 1)));
  }
  void move(int direction, uint32_t now) {
    from = offset(now) + direction;
    started = now;
  }
  void reset() { from = 0; }
};
template <class Canvas>
void selection(Canvas& c, Rect r, bool selected, float hold = 0) {
  c.setDrawColor(2);
  int fill = selected ? r.w - 2 : pixel((r.w - 2) * limit(hold, 0, 1));
  if (fill > 0 && r.h > 2) c.drawBox(r.x + 1, r.y + 1, fill, r.h - 2);
  c.setDrawColor(1);
  c.drawFrame(r.x, r.y, r.w, r.h);
}
template <class Canvas>
void centeredText(Canvas& c, Rect r, int baseline, const char* text) {
  if (!text || !*text) return;
  int bearing = c.getXOffsetGlyph(static_cast<uint8_t>(*text));
  // GetStrWidth includes a positive first bearing twice, but a negative one once.
  // Center the visible ink, then recover the glyph's drawing origin.
  int inkWidth = c.getStrWidth(text) - bearing - (bearing > 0 ? bearing : 0);
  int x = r.x + (r.w - inkWidth) / 2 - bearing;
  c.drawStr(x, baseline, text);
}
}  // namespace oled
