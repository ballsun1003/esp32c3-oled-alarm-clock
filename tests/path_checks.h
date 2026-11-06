static void checkPath(const oled::PairPath& p, double speed, double maxJerk) {
  for (int step = 1; step < 4000; step++) {
    double t = p.duration * step / 4000.0, rate = p.rate, ramp = p.ramp;
    double v = rate, a = 0, j = 0;
    if (p.braking && t >= p.brakeAt) {
      double u = (t - p.brakeAt) / ramp;
      v = rate * (1 - 3 * u * u + 2 * u * u * u);
      a = rate / ramp * (-6 * u + 6 * u * u);
      j = rate / ramp / ramp * (-6 + 12 * u);
    } else if (t < ramp) {
      double u = t / ramp;
      v = rate * (3 * u * u - 2 * u * u * u);
      a = rate / ramp * (6 * u - 6 * u * u);
      j = rate / ramp / ramp * (6 - 12 * u);
    } else if (!p.braking && t > p.duration - ramp) {
      double u = (p.duration - t) / ramp;
      v = rate * (3 * u * u - 2 * u * u * u);
      a = -rate / ramp * (6 * u - 6 * u * u);
      j = rate / ramp / ramp * (6 - 12 * u);
    }
    double q = p.parameter(float(t));
    for (int i = 0; i < 2; i++) {
      double dx = p.ex[i] - p.sx[i] + 2 * p.bx * (1 - 2 * q),
             dy = p.ey[i] - p.sy[i] + 2 * p.by * (1 - 2 * q);
      assert(hypot(dx * v, dy * v) <= speed * 1.0001);
      assert(hypot(dx * j - 12 * p.bx * v * a, dy * j - 12 * p.by * v * a) <= maxJerk * 1.0001);
    }
    assert(q >= 0 && q <= 1.0001);
  }
}
static void pathChecks() {
  for (int n = 0; n < 200; n++) {
    float x[] = {0, 60}, y[] = {0, 35}, tx[] = {float(n % 84), float((n * 7) % 64)},
          ty[] = {float((n * 11) % 35), float((n * 13) % 40)};
    float speed = 1 + n % 18, jerk = 40 + (n % 101) * 5.6f;
    oled::PairPath p;
    p.begin(x, y, tx, ty, n % 19 - 9, n % 17 - 8, speed, jerk);
    checkPath(p, speed, jerk);
    assert(p.parameter(0) == 0 && p.parameter(p.duration) == 1);
    p.advance(p.ramp + .01f);
    p.stopSoon();
    p.advance(.001f);
    if (p.braking) {
      checkPath(p, speed, jerk);
      assert(p.parameter(p.duration) < 1);
    }
  }
  // Fine heat integration counts illuminated pixels, not empty box interiors.
  oled::Motion m;
  m.initialize(fixture(0), fixture(1));
  for (int n = 0; n < 100; n++) m.advance(1 / 30.f);
  uint64_t total = 0;
  for (int i = 0; i < 8192; i++) total += m.pixelHeat()[i];
  unsigned lit = 0;
  for (auto& b : m.bodies)
    for (int y = 0; y < b.mask.h; y++)
      for (int x = 0; x < b.mask.w; x++)
        if (b.mask.lit(x, y)) lit++;
  assert(total >= lit * 98 && total <= lit * 101);
  printf(
      "Paths: 200 curves plus braking, analytical speed/jerk bounds and lit-pixel heat accounting "
      "passed (Motion storage %zu B).\n",
      sizeof(oled::Motion));
}
static void pulseChecks() {
  oled::UiAnimation flash, plain;
  uint8_t a[1024] = {}, b[1024] = {};
  flash.begin(1, 1, 0, false);
  plain.begin(1, 1, 0, false);
  flash.focus({10, 10, 40, 14}, 0);
  plain.focus({10, 10, 40, 14}, 0);
  flash.compose(a, 0);
  plain.compose(b, 0);
  flash.activate();
  flash.begin(2, 2, 100, true);
  plain.begin(2, 2, 100, true);
  memset(a, 0xff, 1024);
  memset(b, 0xff, 1024);
  flash.compose(a, 100);
  plain.compose(b, 100);
  assert(memcmp(a, b, 1024) != 0);
  assert(a[127] == 0xff && b[127] == 0xff);  // Slide started too.
  for (int frame = 1; frame <= 12; frame++) {
    memset(a, 0xff, 1024);
    memset(b, 0xff, 1024);
    flash.compose(a, 100 + frame * 16);
    plain.compose(b, 100 + frame * 16);
    assert(memcmp(a, b, 1024) == 0);  // Exactly one rendered pulse, no replay.
  }
  puts("Navigation: one-frame activation pulse simultaneous with first slide displacement passed.");
}
