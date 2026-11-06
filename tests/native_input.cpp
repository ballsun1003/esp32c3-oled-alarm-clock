#include <stdio.h>
#include <stdlib.h>

#include <initializer_list>

#include "../oled_input.h"
#define CHECK(c)                                                 \
  do {                                                           \
    if (!(c)) {                                                  \
      fprintf(stderr, "Input FAIL line %d: %s\n", __LINE__, #c); \
      exit(1);                                                   \
    }                                                            \
  } while (0)
int main() {
  for(unsigned start : {0U,0xfffffff0U})
    for(unsigned hold : {300U,1000U,3000U})
    for(unsigned delay=200;delay<=1000;delay+=50) {
      oled::InputSettings s;s.holdDelayMs=hold;s.repeatDelayMs=delay;
      oled::DebouncedButton button;
      CHECK(!button.update(true,start,s,true));
      CHECK(button.update(true,start+5,s,true));
      CHECK(!button.update(true,start+5+delay-1,s,true));
      CHECK(button.update(true,start+5+delay,s,true));
      CHECK(!button.update(true,start+5+delay+99,s,true));
      CHECK(button.update(true,start+5+delay+100,s,true));
    }
  for (unsigned start : {0U, 0xfffffff0U}) {
    oled::DebouncedButton button;
    oled::InputSettings s;
    for (unsigned ms = 0; ms < 20; ms++) CHECK(!button.update(ms % 2 == 0, start + ms, s, true));
    CHECK(!button.down());
    CHECK(!button.update(true, start + 20, s, true));
    CHECK(!button.update(true, start + 24, s, true));
    CHECK(button.update(true, start + 25, s, true));
    CHECK(button.down());
    int repeats = 0;
    for (unsigned ms = 26; ms <= 1525; ms++)
      if (button.update(true, start + ms, s, true)) repeats++;
    CHECK(repeats == 10);  // Default repeat wait 600ms: 625,725,...1525, no event flood.
    CHECK(!button.update(false, start + 1526, s, true));
    CHECK(button.down());
    CHECK(!button.update(false, start + 1531, s, true));
    CHECK(!button.down());
    oled::DebouncedButton ok;
    CHECK(!ok.update(true, start, s, false));
    CHECK(ok.update(true, start + 5, s, false));
    for (unsigned ms = 6; ms < 5000; ms++) CHECK(!ok.update(true, start + ms, s, false));
    oled::HoldGesture hold;
    hold.begin(17, start, 1200);
    CHECK(hold.fraction(start + 600) == .5f);
    hold.presented(start + 1199);
    CHECK(!hold.poll(true, start + 1200));
    CHECK(!hold.poll(true, start + 1300));
    hold.presented(start + 1300);
    CHECK(hold.poll(true, start + 1301));
    CHECK(!hold.poll(true, start + 3000));
    hold.begin(18, start, 1200);
    CHECK(!hold.poll(false, start + 1199));
    hold.presented(start + 1500);
    CHECK(!hold.poll(true, start + 1600));
    CHECK(!hold.running());
  }
  // Slow-filter setting, bounce after a stable release, and repeat configuration.
  oled::InputSettings s;
  s.debounceMs = 50;
  s.holdDelayMs = 3000;
  s.repeatDelayMs = 300;
  s.repeatHz = 2;
  oled::DebouncedButton b;
  CHECK(!b.update(true, 0, s, true));
  CHECK(!b.update(true, 49, s, true));
  CHECK(b.update(true, 50, s, true));
  CHECK(!b.update(true, 349, s, true));
  CHECK(b.update(true, 350, s, true));
  CHECK(!b.update(true, 849, s, true));
  CHECK(b.update(true, 850, s, true));
  CHECK(!b.update(false, 851, s, true));
  CHECK(!b.update(false, 901, s, true));
  CHECK(!b.down());
  puts(
      "Input: stable debounce, edge-only OK, configurable repeats, hold cancel/full-frame "
      "gate/once and timer rollover passed.");
}
