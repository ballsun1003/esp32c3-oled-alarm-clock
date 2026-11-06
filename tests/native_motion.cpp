#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <initializer_list>
#undef assert
#define assert(condition)                                          \
  do {                                                             \
    if (!(condition)) {                                            \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
      exit(1);                                                     \
    }                                                              \
  } while (0)
#define OLED_MOTION_TEST
#include "../oled_motion.h"
#include "../oled_ui.h"
#include "motion_masks.h"

static oled::Mask fixture(int i) {
  oled::Mask m;
  m.w = maskWidths[i];
  m.h = maskHeights[i];
  memcpy(m.bits, maskBits[i], 512);
  return m;
}
static void valid(const oled::Motion& m) {
  for (auto& b : m.bodies) {if(!oled::inside(b.box()))fprintf(stderr,"outside: %.4f %.4f %ux%u pending=%d next=%ux%u\n",b.x,b.y,b.mask.w,b.mask.h,b.pending,b.next.w,b.next.h);assert(oled::inside(b.box()));}
  assert(!oled::intersects(m.bodies[0].box(), m.bodies[1].box()));
}
static void profile(int speed, int cover, bool organic, unsigned seed, int curve = 55,int jerk=60) {
  oled::Motion m;
  m.settings.speedHalf = speed;
  m.settings.coverage = cover;
  m.settings.curve = curve;
  m.settings.organic = organic;
  m.settings.jerk=jerk;
  m.seed(seed);
  m.initialize(fixture(0), fixture(1));
  unsigned edges[2][4] = {};
  float maxWait = 0, wait = 0;
  int phase = 1;
  bool requested = false;
  float changeAt = 3;
  uint32_t actual[8192] = {};
  for (int frame = 0; frame < 30 * 1200; frame++) {
    float now = frame / 30.f;
    if (!requested && now >= changeAt - 1) {
      phase = phase % 4 + 1;
      m.offer(1, fixture(phase), false);
      requested = true;
    }
    if (requested && now >= changeAt) {
      m.offer(1, fixture(phase), true);
      wait += 1 / 30.f;
    }
    m.advance(1 / 30.f);
    valid(m);
    if (requested && now >= changeAt && !m.bodies[1].pending) {
      if (wait > maxWait) maxWait = wait;
      wait = 0;
      requested = false;
      changeAt = now + 3;
    }
    for (int i = 0; i < 2; i++) {
      auto r = m.bodies[i].box();
      if (!r.x) edges[i][0]++;
      if (r.x + r.w == 128) edges[i][1]++;
      if (!r.y) edges[i][2]++;
      if (r.y + r.h == 64) edges[i][3]++;
      for (int y = 0; y < r.h; y++)
        for (int x = 0; x < r.w; x++)
          if (m.bodies[i].mask.lit(x, y)) actual[(r.y + y) * 128 + r.x + x]++;
    }
  }
  unsigned used = 0, max = 0;
  double sum = 0, square = 0;
  for (uint32_t n : actual) {
    if (n) used++;
    if (n > max) max = n;
    sum += n;
    square += double(n) * n;
  }
  double mean = sum / 8192, cv = sqrt(square / 8192 - mean * mean) / mean;
  printf(
      "speed=%.1f coverage=%d curve=%d organic=%d seed=%u used=%.2f%% peak/mean=%.2f cv=%.3f "
      "exchanges=%u contacts=%u maxWait=%.2f\n",
      speed * .5, cover, curve, organic, seed, used * 100. / 8192, max / mean, cv, m.exchanges,
      m.collisions, maxWait);
  for (int i = 0; i < 2; i++) {
    printf("  body%d edges %u %u %u %u\n", i, edges[i][0], edges[i][1], edges[i][2], edges[i][3]);
    if (organic)
      for (unsigned e : edges[i]) assert(e > 0);
  }
  assert(maxWait < 10 && wait<10);
}
static void growthAndTiming() {
  uint8_t buffer[1024] = {};
  buffer[(13 >> 3) * 128 + 7] |= 1 << (13 & 7);
  buffer[(40 >> 3) * 128 + 64] |= 1 << (40 & 7);
  oled::Mask bounds;
  assert(bounds.capture(buffer));
  assert(bounds.w == 58 && bounds.h == 28);
  assert(bounds.lit(0, 0) && bounds.lit(57, 27));
  oled::Motion a, b;
  a.initialize(fixture(0), fixture(1));
  b.initialize(fixture(0), fixture(1));
  for (int i = 0; i < 30 * 60; i++) a.advance(1 / 30.f);
  for (int i = 0; i < 60 * 60; i++) b.advance(1 / 60.f);
  for (int i = 0; i < 2; i++) {
    assert(fabs(a.bodies[i].x - b.bodies[i].x) < .01);
    assert(fabs(a.bodies[i].y - b.bodies[i].y) < .01);
  }
  oled::Motion m;
  m.initialize(fixture(0), fixture(1));
  m.bodies[1].x = 128 - m.bodies[1].mask.w;
  m.bodies[1].y = 64 - m.bodies[1].mask.h;
  m.offer(1, fixture(5), false);
  for (int i = 0; i < 60; i++) {
    m.advance(1 / 30.f);
    valid(m);
    assert(m.bodies[1].mask.same(fixture(1)));
  }
  m.offer(1, fixture(5), true);
  for (int i = 0; i < 30 * 100 && m.bodies[1].pending; i++) {
    m.advance(1 / 30.f);
    valid(m);
  }
  assert(!m.bodies[1].pending);
}
static void liveSpeed() {
  oled::Motion motion;
  motion.settings.speedHalf = 2;
  motion.initialize(fixture(0), fixture(1));
  for (int i = 0; i < 30 * 100 && !motion.exchanging(); ++i) motion.advance(1 / 30.f);
  assert(motion.exchanging());
  motion.settings.speedHalf = 36;
  motion.resume(); // Same restart-at-rest as returning from Motion Setup.
  float distance = 0;
  for (int frame = 0; frame < 30 * 5; ++frame) {
    float x[2] = {motion.bodies[0].x, motion.bodies[1].x};
    float y[2] = {motion.bodies[0].y, motion.bodies[1].y};
    motion.advance(1 / 30.f);
    valid(motion);
    for (int i = 0; i < 2; i++)
      distance += hypotf(motion.bodies[i].x - x[i], motion.bodies[i].y - y[i]);
  }
  assert(distance > 20);
}
static void transitions() {
  uint8_t frame[1024];
  oled::UiAnimation ui;
  memset(frame, 0x55, 1024);
  ui.begin(0, 0, 0, false);
  ui.compose(frame, 0);
  ui.begin(1, 1, 10, true);
  memset(frame, 0xaa, 1024);
  ui.compose(frame, 80);
  ui.begin(2, 2, 80, true);
  memset(frame, 0xf0, 1024);
  ui.compose(frame, 100);
  ui.begin(1, 1, 100, true);
  memset(frame, 0xaa, 1024);
  ui.compose(frame, 300);
  for (uint8_t p : frame) assert(p == 0xaa);
  oled::Rect r = {2, 19, 34, 13};
  auto before = ui.focus(r, 300), after = ui.focus(r, 310);
  assert(memcmp(&before, &after, sizeof(r)) == 0);
  // Unsigned timer wrap is also used on a device after ~49.7 days.
  ui.begin(4, 2, 0xfffffff0U, true);
  memset(frame, 0x11, 1024);
  ui.compose(frame, 200);
  for (uint8_t p : frame) assert(p == 0x11);
}
static void contentUpdatesPreserveMotion() {
  for (int speed : {2, 12, 36})
    for (unsigned seed : {731U, 912U}) {
      oled::Motion stable;
      stable.settings.speedHalf = speed;
      stable.seed(seed);
      stable.initialize(fixture(0), fixture(1));
      for (int frame = 0; frame < 120 * 100 && !stable.exchanging(); frame++)
        stable.advance(1 / 120.f);
      assert(stable.exchanging());
      oled::Motion changing = stable;
      auto first = fixture(1), second = first;
      second.bits[128 + 10] ^= 2;  // A different digit interior with identical lit bounds.
      int frames = 0;
      do {
        if (frames > 0 && frames % 240 == 0) changing.offer(1, (frames / 240) % 2 ? second : first);
        stable.advance(1 / 120.f);
        changing.advance(1 / 120.f);
        valid(stable);
        valid(changing);
        for (int i = 0; i < 2; i++) {
          assert(fabsf(stable.bodies[i].x - changing.bodies[i].x) < .0001f);
          assert(fabsf(stable.bodies[i].y - changing.bodies[i].y) < .0001f);
        }
        assert(stable.exchanging() == changing.exchanging());
        assert(++frames < 120 * 200);
      } while (stable.exchanging());
      printf("Content update: speed=%.1f seed=%u identical positions/velocity for %.3fs\n",
             speed * .5, seed, frames / 120.0);
    }
}
static bool sameRect(oled::Rect a, oled::Rect b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static void focusIntermediateFrames() {
  // All real alarm-edit fields, including the minute -> Repeat regression.
  const oled::Rect fields[] = {{10, 19, 34, 13}, {52, 16, 28, 18}, {86, 16, 28, 18},
                               {2, 35, 82, 13},  {86, 35, 40, 13}, {2, 50, 40, 13},
                               {46, 50, 32, 13}, {82, 50, 44, 13}};
  unsigned checked = 0;
  for (auto from : fields)
    for (auto to : fields) {
      oled::UiAnimation ui;
      ui.focus(from, 0);
      ui.focus(to, 200);
      assert(sameRect(ui.current(200), from));
      for (int ms = 0; ms <= 120; ms++) {
        auto r = ui.current(200 + ms);
        assert(oled::inside(r) && r.w > 0 && r.h > 0);
        checked++;
      }
      assert(sameRect(ui.current(320), to));
      // Enter edit before motion completes: the outline must enclose the field.
      ui.focus(from, 400);
      assert(sameRect(ui.focus(from, 417, true), from));
      assert(sameRect(ui.focus(from, 430, false), from));
    }
  oled::UiAnimation ui;
  uint32_t now = 0xfffffff0U;
  ui.focus(fields[0], now);
  for (int i = 0; i < 1000; i++) {
    now += 17;
    auto before = ui.current(now);
    auto after = ui.focus(fields[(i * 5 + 1) % 8], now);
    assert(sameRect(before, after) && oled::inside(after));
    for (int ms = 1; ms <= 17; ms++) assert(oled::inside(ui.current(now + ms)));
  }
  printf("Focus: %u intermediate frames plus 1000 interrupted moves/rollover passed\n", checked);
}
#include "path_checks.h"
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  growthAndTiming();
  liveSpeed();
  transitions();
  pathChecks();
  pulseChecks();
  focusIntermediateFrames();
  contentUpdatesPreserveMotion();
  for (int speed : {2, 12, 36})
    for (int cover : {0, 100}) profile(speed, cover, true, 731);
  profile(12, 70, false, 83);
  profile(12, 100, true, 912, 0);
  profile(36, 100, true, 417, 100);
  profile(2,100,true,85,100,100);
  profile(12,100,true,912,100,100);
  profile(36,100,true,731,100,100);
  profile(12,100,true,85,55,0);
  puts("Native motion and transition checks passed.");
}
