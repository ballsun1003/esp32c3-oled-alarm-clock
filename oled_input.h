#pragma once
#include <stdint.h>

namespace oled {
struct InputSettings {
  static constexpr uint16_t MIN_HOLD_MS = 300, MAX_HOLD_MS = 3000;
  static constexpr uint16_t MIN_REPEAT_MS = 200, MAX_REPEAT_MS = 1000;
  uint16_t debounceMs = 5, holdDelayMs = 1000, repeatDelayMs = 600;
  uint8_t repeatHz = 10, chordMs = 60;
};
class DebouncedButton {
  bool raw = false, stable = false;
  uint32_t changed = 0, pressed = 0, repeated = 0;
  bool repeating = false;

 public:
  bool update(bool down, uint32_t now, const InputSettings& s, bool allowRepeat) {
    if (raw != down) {
      raw = down;
      changed = now;
    }
    if (raw != stable && uint32_t(now - changed) >= s.debounceMs) {
      stable = raw;
      if (stable) {
        pressed = repeated = now;
        repeating = false;
        return true;
      }
    }
    if (!stable || !raw || !allowRepeat) return false;
    uint32_t interval = repeating ? 1000U / (s.repeatHz ? s.repeatHz : 1) : s.repeatDelayMs;
    if (uint32_t(now - repeated) >= interval) {
      repeated = now;
      repeating = true;
      return true;
    }
    return false;
  }
  bool down() const { return stable; }
  uint32_t heldFor(uint32_t now) const { return stable ? uint32_t(now - pressed) : 0; }
};
class HoldGesture {
  uint32_t started = 0, duration = 1;
  bool active = false, fullPresented = false;

 public:
  int key = -1;
  void begin(int source, uint32_t now, uint32_t milliseconds) {
    key = source;
    started = now;
    duration = milliseconds ? milliseconds : 1;
    active = true;
    fullPresented = false;
  }
  void cancel() {
    active = false;
    key = -1;
    fullPresented = false;
  }
  bool running() const { return active; }
  float fraction(uint32_t now) const {
    if (!active) return 0;
    uint32_t elapsed = now - started;
    return elapsed >= duration ? 1.f : float(elapsed) / duration;
  }
  void presented(uint32_t now) {
    if (active && fraction(now) >= 1) fullPresented = true;
  }
  bool poll(bool down, uint32_t now) {
    if (!active) return false;
    if (!down) {
      cancel();
      return false;
    }
    if (fullPresented && uint32_t(now - started) >= duration) {
      cancel();
      return true;
    }
    return false;
  }
};
}  // namespace oled
