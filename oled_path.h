#pragma once
#include <math.h>
namespace oled {
// A shared quadratic bend preserves the separation of the two rectangles.
// The scalar speed ramps with a cubic smoothstep: velocity and acceleration
// are continuous, and duration explicitly bounds jerk (including the bend).
struct PairPath {
  float sx[2] = {}, sy[2] = {}, ex[2] = {}, ey[2] = {}, bx = 0, by = 0;
  float elapsed = 0, duration = 1, ramp = 1, rate = 1;
  bool active = false;
  bool stopping = false, braking = false;
  float brakeAt = 0, brakeP = 0;
  void begin(const float* x, const float* y, const float* tx, const float* ty, float bowX,
             float bowY, float speed, float maxJerk) {
    float distance = 0, bow = hypotf(bowX, bowY);
    for (int i = 0; i < 2; i++) {
      sx[i] = x[i];
      sy[i] = y[i];
      ex[i] = tx[i];
      ey[i] = ty[i];
      distance = fmaxf(distance, hypotf(tx[i] - x[i], ty[i] - y[i]));
    }
    bx = bowX;
    by = bowY;
    float length = fmaxf(.01f, distance + 2 * bow);
    rate = fminf(speed / length, cbrtf(maxJerk / (12 * length)));
    if (bow > .001f) rate = fminf(rate, cbrtf(maxJerk / (36 * bow)));
    ramp = fmaxf(sqrtf(12 * length * rate / maxJerk), 36 * bow * rate * rate / maxJerk);
    duration = 1 / rate + ramp;
    elapsed = 0;
    active = true;
    stopping = braking = false;
  }
  float parameter(float t) const {
    if (braking && t >= brakeAt) {
      float u = fminf(1, (t - brakeAt) / ramp);
      return brakeP + rate * ramp * (u - u * u * u + .5f * u * u * u * u);
    }
    if (t <= 0) return 0;
    if (t >= duration) return 1;
    bool falling = t > duration - ramp;
    if (falling) t = duration - t;
    if (t < ramp) {
      float u = t / ramp;
      float p = rate * ramp * (u * u * u - .5f * u * u * u * u);
      return falling ? 1 - p : p;
    }
    return rate * (t - .5f * ramp);
  }
  void position(int i, float p, float& x, float& y) const {
    float curve = 2 * p * (1 - p);
    x = sx[i] + (ex[i] - sx[i]) * p + bx * curve;
    y = sy[i] + (ey[i] - sy[i]) * p + by * curve;
  }
  void stopSoon() { stopping = true; }
  void advance(float dt) {
    if (stopping && !braking && elapsed >= ramp && elapsed < duration - ramp) {
      brakeP = parameter(elapsed);
      brakeAt = elapsed;
      braking = true;
      duration = elapsed + ramp;
    }
    elapsed = fminf(duration, elapsed + dt);
    if (elapsed >= duration) active = false;
  }
};
}  // namespace oled
