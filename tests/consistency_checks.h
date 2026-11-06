// Regressions use the extracted firmware renderers and installed U8g2 fonts.
static bool pixelIn(const uint8_t* frame, int x, int y) {
  return (frame[(y >> 3) * 128 + x] >> (y & 7)) & 1;
}
static void cleanCanvas();
static oled::Rect inkBounds();
static void rawCentered(const char* text, oled::Rect box, int baseline);
static void digitChecks() {
  struct Case {
    const char* from;
    const char* to;
    const uint8_t* font;
    int height;
  };
  Case cases[] = {
      {"12:34", "12:35", u8g2_font_6x10_tr, 9},   {"1000", "1050", u8g2_font_ncenB14_tr, 15},
      {" 950", "1000", u8g2_font_ncenB14_tr, 15}, {"1000", " 950", u8g2_font_ncenB14_tr, 15},
      {"23", "00", u8g2_font_ncenB12_tr, 12},     {"59", "00", u8g2_font_ncenB12_tr, 12},
      {" 9.5", "10.0", u8g2_font_6x10_tr, 9},     {" 95%", "100%", u8g2_font_6x10_tr, 9},
      {" 950ms", "1000ms", u8g2_font_6x10_tr, 9}, {" 9 min", "10 min", u8g2_font_6x10_tr, 9},
      {"10/s", " 9/s", u8g2_font_6x10_tr, 9},     {" 1m", "Off", u8g2_font_6x10_tr, 9},
      {"Off", " 1m", u8g2_font_6x10_tr, 9}};
  unsigned frames = 0;
  for (const auto& c : cases)
    for (int direction : {-1, 1}) {
      oled::RollingText roll;
      oled::Rect clip = {20, 30 - c.height, 100, c.height + 2};
      u8g2.setFont(c.font);
      int cell = u8g2.getStrWidth("00") - u8g2.getStrWidth("0");
      uint32_t now = 0xfffffff0U;
      u8g2.clear();
      roll.draw(u8g2, 7, c.from, 20, 30, clip, direction, now, true);
      uint8_t before[1024];
      memcpy(before, u8g2.bytes, 1024);
      now += 100;
      for (int ms : {0, 16, 20, 32, 39, 40, 60}) {
        u8g2.clear();
        roll.draw(u8g2, 7, c.to, 20, 30, clip, direction, now + ms, true);
        uint8_t actual[1024];
        memcpy(actual, u8g2.bytes, 1024);
        bool numeric = c.from[0] != 'O' && c.to[0] != 'O';
        if (numeric)
          for (int i = 0; c.to[i]; i++)
            if (c.from[i] == c.to[i])
              for (int y = 0; y < 64; y++)
                for (int x = 20 + i * cell; x < 20 + (i + 1) * cell; x++)
                  assert(pixelIn(before, x, y) == pixelIn(actual, x, y));
        for (int y = 0; y < 64; y++)
          for (int x = 0; x < 128; x++)
            if (x < clip.x || x >= clip.x + clip.w || y < clip.y || y >= clip.y + clip.h)
              assert(!pixelIn(actual, x, y));
        if (ms >= 40 || !numeric) {
          u8g2.clear();
          // Independent, unclipped U8g2 text. Leading reserved digit slots use
          // the font's actual advance, not RollingText's own implementation.
          int leading = 0;
          while (c.to[leading] == ' ') leading++;
          int advance = u8g2_GetGlyphWidth(&u8g2.g, '0');
          u8g2.drawStr(20 + leading * advance, 30, c.to + leading);
          assert(memcmp(actual, u8g2.bytes, 1024) == 0);
        }
        frames++;
      }
    }
  printf(
      "Digits: %u carry/wrap/unit/Off frames, unchanged-place identity and clip bounds passed.\n",
      frames);
}
static void keyboardConsistencyChecks() {
  const char* sets[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789",
                        "!@#$%^&*()_+-=[]{}|;':\",./<>?"};
  unsigned glyphs = 0;
  for (int mode = 0; mode < 5; mode++) {
    textInputMgr.reset("abc", 64);
    for (int m = 0; m < mode; m++) textMode();
    const char* commands[] = {"DEL", "OK", "CLR", "ESC", "CUR"};
    int count = mode == 4 ? 5 : strlen(sets[mode]);
    for (int i = 0; i < count; i++) {
      displayTime += 100;
      u8g2.clear();
      textInputMgr.draw();
      uint8_t rest[1024];
      memcpy(rest, u8g2.bytes, 1024);
      char letter[] = {mode == 4 ? ' ' : sets[mode][i], 0};
      const char* label = mode == 4 ? commands[i] : letter;
      u8g2.setFont(mode == 4 ? u8g2_font_6x10_tr : u8g2_font_ncenB12_tr);
      rawCentered(label, {54, 33, 21, 24}, mode == 4 ? 49 : 51);
      auto ink = inkBounds();
      assert(abs((ink.x - 55) - (73 - ink.x - ink.w)) <= 1);
      for (int y = 30; y < 60; y++)
        for (int x = 45; x < 83; x++) {
          if (u8g2.lit(x, y)) assert(x >= 56 && x <= 72 && y >= 35 && y <= 54);
          if (x > 54 && x < 74 && y > 33 && y < 56) assert(u8g2.lit(x, y) == pixelIn(rest, x, y));
        }
      // All modes keep the 21x24 box, with exactly one full-interior pulse.
      textInputMgr.processInput(true);
      u8g2.clear();
      textInputMgr.draw();
      for (int y = 33; y < 57; y++)
        for (int x = 54; x < 75; x++) {
          bool edge = x == 54 || x == 74 || y == 33 || y == 56;
          assert(u8g2.lit(x, y) == (edge ? true : !pixelIn(rest, x, y)));
        }
      if (mode == 4 && i == 4) textInputMgr.processInput(true);  // Leave cursor mode.
      u8g2.clear();
      textInputMgr.draw();  // Consume any cursor-exit acknowledgement.
      u8g2.clear();
      textInputMgr.draw();
      for (int y = 33; y < 57; y++)
        for (int x = 54; x < 75; x++) assert(u8g2.lit(x, y) == pixelIn(rest, x, y));
      if (mode == 4 && i == 0) save("consistency-keyboard-cmd");
      textStep(1);
      glyphs++;
    }
  }
  printf("Keyboard: %u glyph/command bounds, fixed CMD box and one-frame full fill passed.\n",
         glyphs);
}
static void navigationConsistencyChecks() {
  // All AP connection/scan status pairs preserve the page, scroll and marquee key.
  for (UIState from : {WIFI_LIST, WIFI_SCANNING, WIFI_CONNECTING})
    for (UIState to : {WIFI_LIST, WIFI_SCANNING, WIFI_CONNECTING}) {
      assert(uiPage(from) == uiPage(to));
      oled::UiAnimation nav;
      uint8_t frame[1024] = {};
      nav.begin(uiPage(from), 2, 0, false);
      nav.focus({0, 15, 128, 14}, 0);
      nav.compose(frame, 0);
      nav.begin(uiPage(to), 2, 16, true);
      memset(frame, 0xa5, 1024);
      nav.compose(frame, 16);
      for (uint8_t byte : frame) assert(byte == 0xa5);
    }
  assert(uiPage(WIFI_PASS_INPUT) != uiPage(WIFI_LIST));
  assert(uiPage(MAIN_MENU) != uiPage(WIFI_LIST));
  oled::UiAnimation flash, plain;
  uint8_t a[1024] = {}, b[1024] = {};
  oled::Rect visible;
  for (auto* nav : {&flash, &plain}) {
    nav->begin(1, 1, 0, false);
    nav->focus({0, 3, 128, 13}, 0);
    nav->compose(a, 0);
    nav->focus({0, 27, 128, 13}, 100);
    visible = nav->focus({0, 27, 128, 13}, 116);
    memset(a, 0, 1024);
    nav->compose(a, 116);
  }
  flash.activate();
  flash.begin(2, 2, 117, true);
  plain.begin(2, 2, 117, true);
  memset(a, 0, 1024);
  memset(b, 0, 1024);
  flash.compose(a, 117);
  plain.compose(b, 117);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 127; x++) {
      bool in = x + 1 > visible.x && x + 1 < visible.x + visible.w - 1 && y > visible.y &&
                y < visible.y + visible.h - 1;
      assert((pixelIn(a, x, y) != pixelIn(b, x, y)) == in);
    }
  // Explicit keyboard acknowledgement works with a page slide or an in-place loading overlay.
  for (bool slide : {false, true}) {
    oled::UiAnimation nav;
    uint8_t frame[1024] = {};
    nav.begin(3, 3, 0, false);
    nav.compose(frame, 0);
    nav.activate(TextInputManager::selectorBox());
    nav.begin(101, 3, 16, slide);
    nav.compose(frame, 16);
    int shift = slide ? 1 : 0;
    assert(pixelIn(frame, 55 - shift, 34) && pixelIn(frame, 73 - shift, 55));
    memset(frame, 0, 1024);
    nav.compose(frame, 32);
    for (uint8_t byte : frame) assert(byte == 0);
  }
  // A command can be submitted before the keyboard's entering slide settles.
  {
    oled::UiAnimation nav;
    uint8_t frame[1024] = {};
    nav.begin(1, 1, 0, false);
    nav.compose(frame, 0);
    nav.begin(3, 3, 1, true);
    nav.compose(frame, 101);
    int enteringShift = 128 - oled::pixel(128 * oled::easeOut(100 / 180.f));
    nav.activate(TextInputManager::selectorBox());
    nav.begin(2, 2, 102, true);
    nav.compose(frame, 102);
    assert(pixelIn(frame, 55 + enteringShift + 1, 34));
    assert(!pixelIn(frame, 55, 34));
  }
  puts(
      "Navigation: in-place WiFi status, quick-click visible cursor and keyboard exit/overlay "
      "pulses passed.");
}
static void consistencyChecks() {
  digitChecks();
  keyboardConsistencyChecks();
  navigationConsistencyChecks();
  state.isEditing = false;
  uiHold.cancel();
  settleFocus = true;
  for (int index = 0; index < 3; index++) {
    state.locMenuIdx = index;
    state.uiState = LOCATION_SETUP;
    u8g2.clear();
    display.drawLocation();
    display.verify("location setup");
    assert(display.focus.y == 28 + index * 12);
    display.finish();
    if (index == 2) save("consistency-location");
  }
  oled::Rect r = {10, 20, 42, 14};
  for (float fraction : {0.f, .25f, .5f, .75f, 1.f}) {
    u8g2.clear();
    oled::selection(u8g2, r, false, fraction);
    int fill = oled::pixel((r.w - 2) * fraction);
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) {
        bool edge = x == r.x || x == r.x + r.w - 1 || y == r.y || y == r.y + r.h - 1;
        assert(u8g2.lit(x, y) == (edge || x < r.x + 1 + fill));
      }
  }
  for (int value : {500, 950, 1000, 3000, 3050}) {
    state.co2Limit = value;
    state.uiState = CO2_SETTING;
    display.queued = false;
    displayTime += 100;
    u8g2.clear();
    display.drawCo2Setting();
    assert(!display.queued);
    displayTime += 60;
    u8g2.clear();
    display.drawCo2Setting();
    save(value == 3050 ? "consistency-co2-off" : "consistency-co2");
    if (value == 3050) {
      uint8_t actual[1024];
      memcpy(actual, u8g2.bytes, 1024);
      u8g2.clear();
      u8g2.setFont(u8g2_font_ncenB14_tr);
      u8g2.drawStr((128 - u8g2.getStrWidth("Off")) / 2, 47, "Off");
      for (int y = 20; y < 50; y++)
        for (int x = 40; x < 90; x++)
          if (u8g2.lit(x, y)) {
            assert(y >= 32 && y <= 46);
            assert(pixelIn(actual, x, y));
          }
    }
  }
  // Editing and holding during list motion must bring the row and its box together.
  settleFocus = false;
  for (bool hold : {false, true})
    for (UIState page : {MOTION_SETTING, INPUT_SETTING}) {
      state.uiState = page;
      state.motionIdx = state.inputIdx = 4;
      state.motionOffset = state.inputOffset = 0;
      state.isEditing = false;
      display.scrollView = -1;
      displayTime = 10000;
      u8g2.clear();
      if (page == MOTION_SETTING)
        display.drawMotionSettings();
      else
        display.drawInputSettings();
      display.finish();
      state.motionIdx = state.inputIdx = 5;
      state.motionOffset = state.inputOffset = 1;
      displayTime += 16;
      u8g2.clear();
      if (page == MOTION_SETTING)
        display.drawMotionSettings();
      else
        display.drawInputSettings();
      display.finish();
      displayTime += 1;
      if (hold)
        uiHold.begin(page * 1000 + 5, displayTime, 1200);
      else
        state.isEditing = true;
      u8g2.clear();
      if (page == MOTION_SETTING)
        display.drawMotionSettings();
      else
        display.drawInputSettings();
      display.verify("mid-scroll edit/hold");
      assert(display.focus.y == 51);
      display.finish();
      auto actual = display.animation.current(displayTime);
      assert(actual.y == 51 && actual.h == 13);
      if (!hold && page == MOTION_SETTING) save("consistency-scroll-edit");
      uiHold.cancel();
      state.isEditing = false;
    }
  settleFocus = true;
  locResults = {{"Gypsy jumping City with a very long name", "Long Country", "", ""}};
  state.uiState = LOCATION_RESULT_LIST;
  state.listIdx = 1;
  state.listOffset = 0;
  display.scrollView = -1;
  display.marqueeFocusKey = -1;
  for (int ms : {0, 1000, 1500, 3000, 5000}) {
    displayTime = 20000 + ms;
    display.marqueeFrameSerial++;
    u8g2.clear();
    display.drawLocationResultList();
    display.verify("long city marquee");
    display.finish();
  }
  save("consistency-city");
  for (const char* message : {"A very long AP name that exceeds the screen\nConnecting...",
                              "Updating Data...", "No Saved WiFi\nPlease Setup Wifi"}) {
    displayTime = 420;
    u8g2.clear();
    display.drawLoading(message, 40, true);
    for (int y = 0; y < 64; y++) assert(!u8g2.lit(0, y) && !u8g2.lit(127, y));
    if (message[0] == 'A') save("consistency-long-loading");
  }
  state.isSensorError = true;
  u8g2.clear();
  display.drawCO2(8, 24, 0);
  save("consistency-sensor-error");
  state.isSensorError = false;
  puts(
      "Consistency: frame fill, location divider, decorative CO2 frame, scrolling edits/holds and "
      "long text passed.");
}
