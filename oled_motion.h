#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "oled_path.h"

// Hardware-independent, fixed-storage motion and pixel bounds. No frame-rate
// dependent velocities, framebuffer heap allocations, or per-pixel float map.
namespace oled {
inline float limit(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int pixel(float v) { return static_cast<int>(floorf(v + .5f)); }
struct Rect {
  int x, y, w, h;
};
inline bool intersects(Rect a, Rect b, int gap = 0) {
  return a.x < b.x + b.w + gap && b.x < a.x + a.w + gap && a.y < b.y + b.h + gap &&
         b.y < a.y + a.h + gap;
}
inline bool inside(Rect r) { return r.x >= 0 && r.y >= 0 && r.x + r.w <= 128 && r.y + r.h <= 64; }
struct Mask {
  uint8_t bits[512] = {};
  uint8_t w = 1, h = 1;
  bool lit(int x, int y) const { return bits[(y >> 3) * 128 + x] & (1U << (y & 7)); }
  // Scan the actual U8g2 SSD1306 framebuffer, then crop to its lit extrema.
  bool capture(const uint8_t* buffer) {
    int left = 128, right = -1, top = 64, bottom = -1;
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 128; x++)
        if (buffer[(y >> 3) * 128 + x] & (1U << (y & 7))) {
          if (x < left) left = x;
          if (x > right) right = x;
          if (y < top) top = y;
          if (y > bottom) bottom = y;
        }
    if (right < left || bottom - top + 1 > 32) return false;
    memset(bits, 0, sizeof(bits));
    w = right - left + 1;
    h = bottom - top + 1;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        if (buffer[((y + top) >> 3) * 128 + x + left] & (1U << ((y + top) & 7)))
          bits[(y >> 3) * 128 + x] |= 1U << (y & 7);
    return true;
  }
  bool same(const Mask& b) const {
    return w == b.w && h == b.h && memcmp(bits, b.bits, sizeof(bits)) == 0;
  }
};
struct MotionSettings {
  uint8_t speedHalf = 12;  // 0.5 px/s units, persisted as integers.
  uint8_t curve = 55, coverage = 70, cycleSeconds = 3;
  uint8_t jerk = 60;
  bool organic = true;
};
struct Body {
  Mask mask, next;
  float x = 0, y = 0, vx = 0, vy = 0, gx = 0, gy = 0, goalAge = 100;
  bool pending = false, ready = false, mayCommit = true;
  Rect box() const { return {pixel(x), pixel(y), mask.w, mask.h}; }
};
class Motion {
  uint32_t rng = 0x91e10da5, exposure[128] = {};  // 16x8 bins, shared by both objects.
  uint16_t pixelExposure[8192] = {};
  PairPath path;
  bool usedSmooth = false;
  uint8_t nextAxis = 0;
  uint8_t plannedW[2] = {}, plannedH[2] = {};
  float remainder = 0, tickTime = 0, heatAge = 0, swapAge = 0, blockedAge = 0;
  int exchange = -1, axis = 0, firstSide = 0;
  float startX[2] = {}, startY[2] = {}, endX[2] = {}, endY[2] = {}, progress = 0, duration = 1;
  float laneInset[4] = {};
  bool preparingGrowth = false;
  uint8_t previousSpeedHalf = 12;
  float edgeAge[2][4] = {{}};
  float random01() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng & 0xffffff) / 16777216.f;
  }
  float width(int i) const {
    return bodies[i].pending ? fmaxf(bodies[i].mask.w, bodies[i].next.w) : bodies[i].mask.w;
  }
  float height(int i) const {
    return bodies[i].pending ? fmaxf(bodies[i].mask.h, bodies[i].next.h) : bodies[i].mask.h;
  }
  float footprint(const Mask& m, float x, float y) const {
    uint64_t sum = 0;
    unsigned count = 0;
    const int bx = pixel(x), by = pixel(y);
    // Sample every lit pixel: blank areas inside the bounding rectangle do not count.
    for (int yy = 0; yy < m.h; yy++)
      for (int xx = 0; xx < m.w; xx++)
        if (m.lit(xx, yy)) {
          int px = bx + xx, py = by + yy;
          if (px < 0)
            px = 0;
          else if (px > 127)
            px = 127;
          if (py < 0)
            py = 0;
          else if (py > 63)
            py = 63;
          sum += exposure[(py >> 3) * 16 + (px >> 3)];
          ++count;
        }
    return count ? static_cast<float>(sum) / count : 0;
  }
  float pixelFootprint(const Mask& m, float x, float y) const {
    uint64_t sum = 0;
    unsigned count = 0;
    int px = pixel(x), py = pixel(y);
    for (int yy = 0; yy < m.h; yy++)
      for (int xx = 0; xx < m.w; xx++)
        if (m.lit(xx, yy)) {
          int sx = static_cast<int>(limit(px + xx, 0, 127)),
              sy = static_cast<int>(limit(py + yy, 0, 63));
          uint32_t n = pixelExposure[sy * 128 + sx];
          sum += n * n;
          ++count;
        }
    return count ? float(sum) / count : 0;
  }
  bool pathFits(int i, const Mask& mask) const {
    if (!path.active) return true;
    if (mask.w == bodies[i].mask.w && mask.h == bodies[i].mask.h) return true;
    if (path.braking) return false;
    float p = path.parameter(path.elapsed);
    float cx = path.ex[i] + ((path.sx[i] + path.ex[i]) * .5f + path.bx - path.ex[i]) * (1 - p);
    float cy = path.ey[i] + ((path.sy[i] + path.ey[i]) * .5f + path.by - path.ey[i]) * (1 - p);
    if (cx < 0 || cy < 0 || cx > 128 - mask.w || cy > 64 - mask.h || path.ex[i] < 0 ||
        path.ey[i] < 0 || path.ex[i] > 128 - mask.w || path.ey[i] > 64 - mask.h)
      return false;
    Rect a = bodies[i].box(), b = bodies[1 - i].box();
    a.w = mask.w;
    a.h = mask.h;
    Rect endA = {pixel(path.ex[i]), pixel(path.ey[i]), mask.w, mask.h};
    Rect endB = {pixel(path.ex[1 - i]), pixel(path.ey[1 - i]), b.w, b.h};
    return (a.x + a.w + 1 <= b.x && endA.x + endA.w + 1 <= endB.x) ||
           (b.x + b.w + 1 <= a.x && endB.x + endB.w + 1 <= endA.x) ||
           (a.y + a.h + 1 <= b.y && endA.y + endA.h + 1 <= endB.y) ||
           (b.y + b.h + 1 <= a.y && endB.y + endB.h + 1 <= endA.y);
  }
  void planSmooth() {
    Rect boxes[2] = {bodies[0].box(), bodies[1].box()};
    // A newly larger weather row gets the shortest safe retreat, rather than
    // waiting for a crossing of the entire OLED. Braking happened beforehand.
    for (int i = 0; i < 2; i++)
      if (bodies[i].pending && (!safe(i, bodies[i].next) || bodies[i].x > 128 - bodies[i].next.w ||
                                bodies[i].y > 64 - bodies[i].next.h)) {
        const Mask& next = bodies[i].next;
        Rect other = boxes[1 - i], current = boxes[i];
        float mx = 128 - next.w, my = 64 - next.h;
        float xs[] = {limit(bodies[i].x, 0, mx), 0, mx, float(other.x - next.w - 2),
                      float(other.x + other.w + 2)};
        float ys[] = {limit(bodies[i].y, 0, my), 0, my, float(other.y - next.h - 2),
                      float(other.y + other.h + 2)};
        float best = 1e30f, tx = 0, ty = 0;
        for (float x : xs)
          for (float y : ys) {
            x = limit(x, 0, mx);
            y = limit(y, 0, my);
            Rect end = {pixel(x), pixel(y), next.w, next.h};
            bool separated =
                (current.x + current.w + 1 <= other.x && end.x + end.w + 1 <= other.x) ||
                (other.x + other.w + 1 <= current.x && other.x + other.w + 1 <= end.x) ||
                (current.y + current.h + 1 <= other.y && end.y + end.h + 1 <= other.y) ||
                (other.y + other.h + 1 <= current.y && other.y + other.h + 1 <= end.y);
            float d = hypotf(x - bodies[i].x, y - bodies[i].y);
            if (separated && d < best) {
              best = d;
              tx = x;
              ty = y;
            }
          }
        if (best < 1e29f && best > .01f) {
          float x[] = {bodies[0].x, bodies[1].x}, y[] = {bodies[0].y, bodies[1].y};
          float ex[] = {x[0], x[1]}, ey[] = {y[0], y[1]};
          ex[i] = tx;
          ey[i] = ty;
          path.begin(x, y, ex, ey, 0, 0, settings.speedHalf * .5f, 600 - settings.jerk * 5.6f);
          for (int j = 0; j < 2; j++) {
            plannedW[j] = width(j);
            plannedH[j] = height(j);
          }
          return;
        }
      }
    bool separated[2] = {
        boxes[0].x + boxes[0].w + 1 <= boxes[1].x || boxes[1].x + boxes[1].w + 1 <= boxes[0].x,
        boxes[0].y + boxes[0].h + 1 <= boxes[1].y || boxes[1].y + boxes[1].h + 1 <= boxes[0].y};
    bool fits[2] = {width(0) + width(1) + 2 <= 128, height(0) + height(1) + 2 <= 64};
    int preserve = (separated[nextAxis] && fits[nextAxis]) ? nextAxis : 1 - nextAxis;
    if (!separated[preserve] || !fits[preserve]) {
      // Current masks still fit. An oversized preview remains pending until a
      // later safe position; it never enlarges the actual collision rectangle.
      preserve = separated[0] ? 0 : 1;
    }
    float widths[2] = {width(0), width(1)}, heights[2] = {height(0), height(1)};
    if (widths[0] + widths[1] + 2 > 128 && preserve == 0)
      for (int i = 0; i < 2; i++) widths[i] = bodies[i].mask.w;
    if (heights[0] + heights[1] + 2 > 64 && preserve == 1)
      for (int i = 0; i < 2; i++) heights[i] = bodies[i].mask.h;
    int first =
            preserve == 0 ? (boxes[0].x < boxes[1].x ? 0 : 1) : (boxes[0].y < boxes[1].y ? 0 : 1),
        second = 1 - first;
    float best = 1e30f, bestX[2] = {}, bestY[2] = {}, bestBowX = 0, bestBowY = 0;
    float x[2] = {bodies[0].x, bodies[1].x}, y[2] = {bodies[0].y, bodies[1].y};
    float average = 1;
    for (uint32_t n : exposure) average += n / 8192.f;
    for (int trial = 0; trial < 16; trial++) {
      float tx[2], ty[2];
      for (int i = 0; i < 2; i++) {
        tx[i] = random01() * (128 - widths[i]);
        ty[i] = random01() * (64 - heights[i]);
      }
      if (preserve == 0) {
        float slack = fmaxf(0, 128 - widths[0] - widths[1] - 2),
              split = widths[first] + 1 + random01() * slack;
        tx[first] = random01() * fmaxf(0, split - widths[first] - 1);
        tx[second] = split + 1 + random01() * fmaxf(0, 128 - widths[second] - split - 1);
        // Exchange vertical places while horizontal separation is reserved.
        if (fits[1]) {
          int top = trial < 4 ? (y[0] > y[1] ? 0 : 1) : trial % 2;
          ty[top] = 0;
          ty[1 - top] = 64 - heights[1 - top];
        }
      } else {
        float slack = fmaxf(0, 64 - heights[0] - heights[1] - 2),
              split = heights[first] + 1 + random01() * slack;
        ty[first] = random01() * fmaxf(0, split - heights[first] - 1);
        ty[second] = split + 1 + random01() * fmaxf(0, 64 - heights[second] - split - 1);
        if (fits[0]) {
          int left = trial < 4 ? (x[0] > x[1] ? 0 : 1) : trial % 2;
          tx[left] = 0;
          tx[1 - left] = 128 - widths[1 - left];
        }
      }
      // Reserve outer edges in some candidates, inner corridors in the rest.
      if (trial < 4) {
        if (preserve == 0) {
          tx[first] = 0;
          tx[second] = 128 - widths[second];
        } else {
          ty[first] = 0;
          ty[second] = 64 - heights[second];
        }
      } else if (preserve == 0 && fits[1]) {
        float space = 64 - heights[0] - heights[1] - 2;
        int top = ty[0] < ty[1] ? 0 : 1;
        float t = random01() * space;
        ty[top] = t;
        ty[1 - top] = t + heights[top] + 2 + random01() * (space - t);
      } else if (preserve == 1 && fits[0]) {
        float space = 128 - widths[0] - widths[1] - 2;
        int left = tx[0] < tx[1] ? 0 : 1;
        float t = random01() * space;
        tx[left] = t;
        tx[1 - left] = t + widths[left] + 2 + random01() * (space - t);
      }
      float bowX = (random01() - .5f) * settings.curve * .18f,
            bowY = (random01() - .5f) * settings.curve * .18f;
      float bowLoX = -1e9f, bowHiX = 1e9f, bowLoY = -1e9f, bowHiY = 1e9f;
      for (int i = 0; i < 2; i++) {
        float midX = (x[i] + tx[i]) * .5f, midY = (y[i] + ty[i]) * .5f;
        bowX = limit(bowX, -midX, 128 - bodies[i].mask.w - midX);
        bowY = limit(bowY, -midY, 64 - bodies[i].mask.h - midY);
        bowLoX = fmaxf(bowLoX, -midX);
        bowHiX = fminf(bowHiX, 128 - widths[i] - midX);
        bowLoY = fmaxf(bowLoY, -midY);
        bowHiY = fminf(bowHiY, 64 - heights[i] - midY);
      }
      // The full Bezier control hull must reserve the next glyph too. An
      // endpoint-only reservation can otherwise postpone a 1px growth until
      // the end of an entire slow crossing, despite room being available now.
      if (bowLoX <= bowHiX) bowX = limit(bowX, bowLoX, bowHiX);
      if (bowLoY <= bowHiY) bowY = limit(bowY, bowLoY, bowHiY);
      PairPath candidate;
      candidate.begin(x, y, tx, ty, bowX, bowY, settings.speedHalf * .5f,
                      600 - settings.jerk * 5.6f);
      float energy = 0, edgeReward = 0;
      for (int i = 0; i < 2; i++) {
        for (int sample = 1; sample <= 7; sample++) {
          float px, py;
          candidate.position(i, candidate.parameter(candidate.duration * sample / 8.f), px, py);
          energy += pixelFootprint(bodies[i].mask, px, py);
        }
        float endpoint =
            pixelFootprint(bodies[i].pending ? bodies[i].next : bodies[i].mask, tx[i], ty[i]);
        energy += endpoint * 2;
        if (pixel(tx[i]) == 0) edgeReward += fminf(edgeAge[i][0] / 30, 3);
        if (pixel(tx[i] + widths[i]) == 128) edgeReward += fminf(edgeAge[i][1] / 30, 3);
        if (pixel(ty[i]) == 0) edgeReward += fminf(edgeAge[i][2] / 30, 3);
        if (pixel(ty[i] + heights[i]) == 64) edgeReward += fminf(edgeAge[i][3] / 30, 3);
      }
      float score = energy / (18 * average * average) * (1 + settings.coverage * .04f) -
                    edgeReward + random01() * .15f;
      if (score < best) {
        best = score;
        for (int i = 0; i < 2; i++) {
          bestX[i] = tx[i];
          bestY[i] = ty[i];
        }
        bestBowX = bowX;
        bestBowY = bowY;
      }
    }
    path.begin(x, y, bestX, bestY, bestBowX, bestBowY, settings.speedHalf * .5f,
               600 - settings.jerk * 5.6f);
    for (int i = 0; i < 2; i++) {
      plannedW[i] = width(i);
      plannedH[i] = height(i);
    }
    nextAxis = 1 - preserve;
    ++exchanges;
  }
  void chooseGoal(int i) {
    Body& b = bodies[i];
    Body& other = bodies[1 - i];
    float maxX = 128 - width(i), maxY = 64 - height(i), best = 1e30f;
    float average = 1;
    for (uint32_t v : exposure) average += v / 128.f;
    const float importance = settings.coverage / 100.f;
    for (int k = 0; k < 28; k++) {
      float x = random01() * maxX, y = random01() * maxY;
      int edge = -1;
      if (k < 20) {
        edge = k % 4;
        if (edge == 0) x = 0;
        if (edge == 1) x = maxX;
        if (edge == 2) y = 0;
        if (edge == 3) y = maxY;
      }
      float distance = hypotf(x - b.x, y - b.y);
      if (distance < 12) continue;
      float energy = footprint(b.pending ? b.next : b.mask, x, y);
      // Include the route, not only its destination, so a cool goal reached
      // through a repeatedly used corridor stops winning the same score.
      for (int sample = 1; sample <= 2; sample++) {
        float t = sample / 3.f;
        energy += .35f * footprint(b.mask, b.x + (x - b.x) * t, b.y + (y - b.y) * t);
      }
      Rect target = {pixel(x), pixel(y), static_cast<int>(width(i)), static_cast<int>(height(i))};
      Rect reserved = {pixel(other.gx), pixel(other.gy), other.mask.w, other.mask.h};
      float conflict = intersects(target, other.box(), 2) ? 2.f : 0.f;
      if (intersects(target, reserved, 2)) conflict += 1.f;
      float score =
          importance * energy / average * 4.f + conflict + random01() * .6f - distance * .006f;
      if (edge >= 0) score -= fminf(edgeAge[i][edge] / 50.f, 2.f);
      if (score < best) {
        best = score;
        b.gx = x;
        b.gy = y;
      }
    }
    b.goalAge = 0;
  }
  bool safe(int i, const Mask& mask) const {
    Rect r = {pixel(bodies[i].x), pixel(bodies[i].y), mask.w, mask.h};
    return inside(r) && !intersects(r, bodies[1 - i].box(), 1);
  }
  void commit() {
    bool resized = false;
    for (int i = 0; i < 2; i++)
      if (bodies[i].pending && bodies[i].mayCommit && safe(i, bodies[i].next) &&
          (!(settings.organic && settings.jerk) || pathFits(i, bodies[i].next))) {
        float oldWidth = width(i), oldHeight = height(i);
        bodies[i].mask = bodies[i].next;
        bodies[i].pending = false;
        if (oldWidth != width(i) || oldHeight != height(i)) {
          bodies[i].goalAge = 100;
          resized = true;
        }
      }
    if (resized && exchange >= 0) stage();
  }
  void stage() {
    float longest = 1;
    float freeX = fmaxf(0, 128 - width(0) - width(1) - 2),
          freeY = fmaxf(0, 64 - height(0) - height(1) - 2);
    float sx = fminf(1, freeX / fmaxf(.01f, laneInset[0] + laneInset[1]));
    float sy = fminf(1, freeY / fmaxf(.01f, laneInset[2] + laneInset[3]));
    for (int i = 0; i < 2; i++) {
      Body& b = bodies[i];
      startX[i] = endX[i] = b.x;
      startY[i] = endY[i] = b.y;
      const int side = (i == 0 ? firstSide : 1 - firstSide);
      float left = laneInset[0] * sx, right = 128 - width(i) - laneInset[1] * sx;
      float top = laneInset[2] * sy, bottom = 64 - height(i) - laneInset[3] * sy;
      if (axis == 0) {
        if (exchange == 0) endX[i] = side ? right : left;
        if (exchange == 1) endY[i] = side ? top : bottom;
        if (exchange == 2) endX[i] = side ? left : right;
      } else {
        if (exchange == 0) endY[i] = side ? bottom : top;
        if (exchange == 1) endX[i] = side ? left : right;
        if (exchange == 2) endY[i] = side ? top : bottom;
      }
      longest = fmaxf(longest, hypotf(endX[i] - b.x, endY[i] - b.y));
    }
    duration = fmaxf(.2f, longest / (settings.speedHalf * .5f));
    progress = 0;
  }
  bool prepareGrowth(float dt) {
    // Before the due time, take the shortest safe retreat for an expanding
    // glyph. Do not wait for a long lane exchange to reach its far endpoint.
    for (int i = 0; i < 2; i++) {
      Body& b = bodies[i];
      if (!b.pending || safe(i, b.next)) continue;
      Rect o = bodies[1 - i].box();
      float mx = 128 - b.next.w, my = 64 - b.next.h;
      float xs[] = {limit(b.x, 0, mx), 0, mx, float(o.x - b.next.w - 1), float(o.x + o.w + 1)};
      float ys[] = {limit(b.y, 0, my), 0, my, float(o.y - b.next.h - 1), float(o.y + o.h + 1)};
      float tx = b.x, ty = b.y, best = 1e30f;
      for (float x : xs)
        for (float y : ys) {
          x = limit(x, 0, mx);
          y = limit(y, 0, my);
          Rect r = {pixel(x), pixel(y), b.next.w, b.next.h};
          if (intersects(r, o, 1)) continue;
          float distance = hypotf(x - b.x, y - b.y);
          if (distance < best) {
            best = distance;
            tx = x;
            ty = y;
          }
        }
      if (best < 1e29f && best > .01f) {
        float step = fminf(1.f, settings.speedHalf * .5f * dt / best);
        float nx = b.x + (tx - b.x) * step, ny = b.y + (ty - b.y) * step;
        Rect r = {pixel(nx), pixel(ny), b.mask.w, b.mask.h};
        if (inside(r) && !intersects(r, o, 1)) {
          b.x = nx;
          b.y = ny;
          preparingGrowth = true;
          return true;
        }
      }
    }
    if (preparingGrowth) {
      preparingGrowth = false;
      if (exchange >= 0) stage();
    }
    return false;
  }
  void beginExchange() {
    // Reserve disjoint passing lanes. Both paths are planned together rather
    // than reversing each velocity independently at a collision.
    bool horizontal = width(0) + width(1) + 2 <= 128;
    bool vertical = height(0) + height(1) + 2 <= 64;
    if (!horizontal && !vertical) return;
    Rect a = bodies[0].box(), b = bodies[1].box();
    bool xSeparated = a.x + a.w <= b.x || b.x + b.w <= a.x;
    axis = horizontal && (xSeparated || !vertical) ? 0 : 1;
    firstSide =
        axis == 0 ? (a.x + a.w * .5f > b.x + b.w * .5f) : (a.y + a.h * .5f > b.y + b.h * .5f);
    // Choose the PAIR of passing lanes from the shared exposure map. Always
    // using the outermost two lanes would create new hot stripes of its own.
    float freeX = fmaxf(0, 128 - width(0) - width(1) - 2),
          freeY = fmaxf(0, 64 - height(0) - height(1) - 2);
    float best = 1e30f;
    for (int trial = 0; trial < (settings.coverage ? 12 : 1); trial++) {
      float l = trial ? random01() * freeX : 0, r = trial ? random01() * (freeX - l) : 0;
      float t = trial ? random01() * freeY : 0, d = trial ? random01() * (freeY - t) : 0;
      if (edgeAge[0][0] > 90 || edgeAge[1][0] > 90) l = 0;
      if (edgeAge[0][1] > 90 || edgeAge[1][1] > 90) r = 0;
      if (edgeAge[0][2] > 90 || edgeAge[1][2] > 90) t = 0;
      if (edgeAge[0][3] > 90 || edgeAge[1][3] > 90) d = 0;
      float energy = 0;
      for (int i = 0; i < 2; i++) {
        const Mask& mask = bodies[i].mask;
        float xs[] = {l, (128 - width(i)) * .5f, 128 - width(i) - r};
        float ys[] = {t, (64 - height(i)) * .5f, 64 - height(i) - d};
        // Endpoints and middle sections of both lanes contribute to the cost.
        for (int j = 0; j < 3; j++) {
          energy += footprint(mask, xs[j], t) + footprint(mask, xs[j], ys[2]);
          energy += footprint(mask, l, ys[j]) + footprint(mask, xs[2], ys[j]);
        }
      }
      float score = energy * (1 + random01() * .15f * (1 - settings.coverage / 100.f));
      if (score < best) {
        best = score;
        laneInset[0] = l;
        laneInset[1] = r;
        laneInset[2] = t;
        laneInset[3] = d;
      }
    }
    exchange = 0;
    ++exchanges;
    stage();
  }
  void exchangeStep(float dt) {
    progress = fminf(1.f, progress + dt / duration);
    // Continuous velocity through the long part; short rounded acceleration
    // and braking avoid spending most of a crossing stalled near a corner.
    float t = progress, r = .12f, p;
    if (t < r)
      p = t * t / (2 * r * (1 - r));
    else if (t > 1 - r)
      p = 1 - (1 - t) * (1 - t) / (2 * r * (1 - r));
    else
      p = (t - r * .5f) / (1 - r);
    float oldX[2] = {bodies[0].x, bodies[1].x}, oldY[2] = {bodies[0].y, bodies[1].y};
    for (int i = 0; i < 2; i++) {
      bodies[i].x = startX[i] + (endX[i] - startX[i]) * p;
      bodies[i].y = startY[i] + (endY[i] - startY[i]) * p;
    }
    if (intersects(bodies[0].box(), bodies[1].box(), 1)) {
      for (int i = 0; i < 2; i++) {
        bodies[i].x = oldX[i];
        bodies[i].y = oldY[i];
      }
      // Growth during a reservation restarts separation using current bounds.
      exchange = -1;
      beginExchange();
      return;
    }
    if (progress >= 1) {
      ++exchange;
      bool canFinish = axis == 0 ? height(0) + height(1) + 2 <= 64 : width(0) + width(1) + 2 <= 128;
      if (exchange >= 3 || (exchange == 2 && !canFinish)) {
        exchange = -1;
        swapAge = 0;
        blockedAge = 0;
        for (int i = 0; i < 2; i++) {
          chooseGoal(i);
          float a = atan2f(bodies[i].gy - bodies[i].y, bodies[i].gx - bodies[i].x);
          bodies[i].vx = cosf(a);
          bodies[i].vy = sinf(a);
        }
      } else
        stage();
    }
  }
  void accumulate() {
    bool rescale = false;
    for (const Body& b : bodies) {
      Rect r = b.box();
      for (int y = 0; y < b.mask.h; y++)
        for (int x = 0; x < b.mask.w; x++)
          if (b.mask.lit(x, y)) {
            exposure[((r.y + y) >> 3) * 16 + ((r.x + x) >> 3)]++;
            uint16_t& n = pixelExposure[(r.y + y) * 128 + r.x + x];
            if (++n >= 60000) rescale = true;
          }
    }
    uint32_t largest = 0;
    for (uint32_t n : exposure)
      if (n > largest) largest = n;
    if (largest > 0x20000000U || rescale) {
      for (uint32_t& n : exposure) n >>= 1;
      for (uint16_t& n : pixelExposure) n >>= 1;
    }
  }
  void step(float dt) {
    tickTime += dt;
    // Keep the phase small on a clock that can remain powered for months.
    // The wrap is an integer number of sine periods, so it has no visible jump.
    constexpr float flowWrap = 400.f * 6.28318530718f / .63f;
    if (tickTime >= flowWrap) tickTime -= flowWrap;
    if (previousSpeedHalf != settings.speedHalf) {
      previousSpeedHalf = settings.speedHalf;
      if (exchange >= 0 && !(settings.organic && settings.jerk)) stage();
    }
    swapAge += dt;
    heatAge += dt;
    for (int i = 0; i < 2; i++) {
      Rect r = bodies[i].box();
      for (float& age : edgeAge[i]) age += dt;
      if (r.x == 0) edgeAge[i][0] = 0;
      if (r.x + r.w == 128) edgeAge[i][1] = 0;
      if (r.y == 0) edgeAge[i][2] = 0;
      if (r.y + r.h == 64) edgeAge[i][3] = 0;
    }
    if (heatAge >= 1.f / 30.f) {
      heatAge -= 1.f / 30.f;
      accumulate();
    }
    if (settings.organic && settings.jerk) {
      if (!usedSmooth) {
        path.active = false;
        exchange = -1;
        usedSmooth = true;
      }
      commit();
      if (!path.active) planSmooth();
      for (int i = 0; i < 2; i++)
        if (bodies[i].pending &&
            (bodies[i].next.w > plannedW[i] || bodies[i].next.h > plannedH[i]) &&
            !pathFits(i, bodies[i].next))
          path.stopSoon();
      path.advance(dt);
      float p = path.parameter(path.elapsed);
      for (int i = 0; i < 2; i++) path.position(i, p, bodies[i].x, bodies[i].y);
      commit();
      return;
    }
    if (usedSmooth) {
      usedSmooth = false;
      path.active = false;
      exchange = -1;
    }
    if (prepareGrowth(dt)) {
      commit();
      return;
    }
    if (exchange >= 0) {
      exchangeStep(dt);
      commit();
      return;
    }
    commit();
    if (settings.organic && swapAge > 45 - settings.coverage * .25f) {
      beginExchange();
      if (exchange >= 0) return;
    }
    float ox[2], oy[2];
    bool predicted = false;
    for (int i = 0; i < 2; i++) {
      Body& b = bodies[i];
      ox[i] = b.x;
      oy[i] = b.y;
      b.goalAge += dt;
      bool navigate = settings.organic || b.pending;
      if (navigate) {
        if (b.goalAge > 18 || hypotf(b.gx - b.x, b.gy - b.y) < 2) chooseGoal(i);
        float angle = atan2f(b.gy - b.y, b.gx - b.x);
        angle += settings.curve * .0025f * sinf(tickTime * .63f + i * 2.4f);
        float heading = atan2f(b.vy, b.vx),
              delta = atan2f(sinf(angle - heading), cosf(angle - heading));
        heading +=
            limit(delta, -dt * (.8f + settings.curve * .025f), dt * (.8f + settings.curve * .025f));
        b.vx = cosf(heading);
        b.vy = sinf(heading);
      }
      float speed = settings.speedHalf * .5f * (i == 1 ? 1.08f : 1.f);
      b.x += b.vx * speed * dt;
      b.y += b.vy * speed * dt;
      float mx = 128 - b.mask.w, my = 64 - b.mask.h;
      if (b.x < 0) {
        b.x = -b.x;
        b.vx = fabsf(b.vx);
      }
      if (b.x > mx) {
        b.x = 2 * mx - b.x;
        b.vx = -fabsf(b.vx);
      }
      if (b.y < 0) {
        b.y = -b.y;
        b.vy = fabsf(b.vy);
      }
      if (b.y > my) {
        b.y = 2 * my - b.y;
        b.vy = -fabsf(b.vy);
      }
      b.x = limit(b.x, 0, mx);
      b.y = limit(b.y, 0, my);
    }
    bool hit = intersects(bodies[0].box(), bodies[1].box(), 1);
    Rect future[2];
    for (int i = 0; i < 2; i++) {
      Body& b = bodies[i];
      float s = settings.speedHalf * .5f * .7f;
      future[i] = {pixel(b.x + b.vx * s), pixel(b.y + b.vy * s), b.mask.w, b.mask.h};
    }
    predicted = intersects(future[0], future[1], 2);
    if (hit) {
      ++collisions;
      for (int i = 0; i < 2; i++) {
        bodies[i].x = ox[i];
        bodies[i].y = oy[i];
      }
      blockedAge += dt;
    }
    if (settings.organic && (predicted || hit)) {
      beginExchange();
    } else if (hit) {
      for (Body& b : bodies) {
        b.vx = -b.vx;
        b.vy = -b.vy;
      }
      if (blockedAge > .3f) beginExchange();
    }
  }

 public:
#ifdef OLED_MOTION_TEST
  const PairPath& plannedPath() const { return path; }
#endif
  MotionSettings settings;
  Body bodies[2];
  uint32_t exchanges = 0, collisions = 0;
  // Called after a menu obscured motion: restart at rest without discarding heat.
  void resume() {
    path.active = false;
    exchange = -1;
  }
  void seed(uint32_t value) { rng = value ? value : 1; }
  void initialize(const Mask& clock, const Mask& env) {
    bodies[0].mask = clock;
    bodies[1].mask = env;
    bodies[0].ready = bodies[1].ready = true;
    bodies[0].x = 0;
    bodies[0].y = 0;
    bodies[1].x = 128 - env.w;
    bodies[1].y = 64 - env.h;
    bodies[0].vx = .8f;
    bodies[0].vy = .6f;
    bodies[1].vx = -.8f;
    bodies[1].vy = -.6f;
    for (int i = 0; i < 2; i++) chooseGoal(i);
  }
  void offer(int i, const Mask& next, bool mayCommit = true) {
    Body& b = bodies[i];
    float oldWidth = width(i), oldHeight = height(i);
    b.mayCommit = mayCommit;
    if (b.mask.same(next)) {
      b.pending = false;
    } else {
      if (b.pending && b.next.same(next)) return;
      b.next = next;
      b.pending = true;
    }
    // Replan only if the reserved bounds change. New digits/pixels within the
    // same bounds must preserve the current crossing's velocity and progress.
    if (oldWidth != width(i) || oldHeight != height(i)) {
      b.goalAge = 100;
      if (exchange >= 0 && !(settings.organic && settings.jerk)) stage();
    }
  }
  void advance(float seconds) {
    if (!bodies[0].ready || !bodies[1].ready) return;
    remainder += seconds;
    while (remainder >= 1.f / 120.f) {
      step(1.f / 120.f);
      remainder -= 1.f / 120.f;
    }
  }
  bool exchanging() const {
    return settings.organic && settings.jerk ? path.active : exchange >= 0;
  }
  const uint32_t* heat() const { return exposure; }
  const uint16_t* pixelHeat() const { return pixelExposure; }
};
}  // namespace oled
