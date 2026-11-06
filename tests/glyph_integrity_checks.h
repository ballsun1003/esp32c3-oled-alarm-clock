// The oracle is an unclipped U8g2 raster, independent of RollingText/centeredText.
static void cleanCanvas() {
  u8g2.setMaxClipWindow();
  u8g2.clear();
}
static oled::Rect inkBounds() {
  int l = 128, r = -1, t = 64, b = -1;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 128; x++)
      if (u8g2.lit(x, y)) {
        l = min(l, x);
        r = max(r, x);
        t = min(t, y);
        b = max(b, y);
      }
  return {l, t, r - l + 1, b - t + 1};
}
static void sameRegion(const uint8_t* expected, oled::Rect region, const char* tag) {
  for (int y = region.y; y < region.y + region.h; y++)
    for (int x = region.x; x < region.x + region.w; x++)
      if (pixelIn(expected, x, y) != u8g2.lit(x, y)) {
        fprintf(stderr, "Glyph mismatch: %s at %d,%d\n", tag, x, y);
        exit(1);
      }
}
static void rawCentered(const char* text, oled::Rect box, int baseline) {
  cleanCanvas();
  u8g2.drawStr(40, baseline, text);
  auto ink = inkBounds();
  int origin = 40 + box.x + (box.w - ink.w) / 2 - ink.x;
  cleanCanvas();
  u8g2.drawStr(origin, baseline, text);
}
static void rawCo2Field(int value) {
  cleanCanvas();
  u8g2.setFont(u8g2_font_ncenB14_tr);
  if (value > 3000)
    rawCentered("Off", {24, 30, 79, 19}, 47);
  else {
    char number[8];
    snprintf(number, sizeof(number), "%d", value);
    u8g2.drawStr(value < 1000 ? 39 : 28, 46, number);
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(72, 45, " ppm");
  }
}
static void co2GlyphChecks() {
  state.uiState = CO2_SETTING;
  state.isEditing = false;
  // Every selectable threshold and all ten digit glyphs, with no rendering helper as oracle.
  unsigned values = 0;
  for (int value = 500; value <= 3050; value += 50) {
    state.co2Limit = value;
    display.rolling.reset();
    displayTime += 100;
    cleanCanvas();
    display.drawCo2Setting();
    uint8_t actual[1024];
    memcpy(actual, u8g2.bytes, 1024);
    rawCo2Field(value);
    // Interior includes the required blank band on all four sides.
    for (int y = 31; y <= 47; y++)
      for (int x = 25; x <= 101; x++)
        if (x == 25 || x == 101 || y == 31 || y == 47) assert(!u8g2.lit(x, y));
    sameRegion(actual, {25, 31, 77, 17}, "CO2 settled field");
    memcpy(u8g2.bytes, actual, 1024);
    if (value == 1050) save("glyph-co2");
    if (value == 3050) save("glyph-co2-off");
    values++;
  }
  for (char digit = '0'; digit <= '9'; digit++) {
    char text[] = {digit, 0};
    cleanCanvas();
    u8g2.setFont(u8g2_font_ncenB14_tr);
    u8g2.drawStr(28, 46, text);
    uint8_t raw[1024];
    memcpy(raw, u8g2.bytes, 1024);
    cleanCanvas();
    oled::RollingText roll;
    roll.draw(u8g2, 1, text, 28, 46, {28, 32, 44, 15}, 1, 0, false);
    sameRegion(raw, {0, 0, 128, 64}, "complete CO2 digit");
  }
  printf("Glyph integrity: %u actual CO2 values and all ten unclipped digit oracles passed.\n",
         values);
}
static void co2AnimationIntegrity() {
  state.uiState = CO2_SETTING;
  state.isEditing = false;
  state.co2Limit = 500;
  displayTime = 40000;
  display.rolling.reset();
  cleanCanvas();
  display.drawCo2Setting();
  unsigned frames = 0;
  for (int direction : {1, -1})
    for (int step = 1; step <= 51; step++) {
      int before = state.co2Limit, next = direction > 0 ? 500 + 50 * step : 3050 - 50 * step;
      rawCo2Field(next);
      uint8_t raw[1024];
      memcpy(raw, u8g2.bytes, 1024);
      char oldDigits[8], newDigits[8];
      snprintf(oldDigits, sizeof(oldDigits), "%4d", before);
      snprintf(newDigits, sizeof(newDigits), "%4d", next);
      state.co2Limit = next;
      state.valueDirection = direction;
      uint32_t start = 40000 + (frames / 5 + 1) * 100;
      for (int ms : {0, 16, 32, 48, 64}) {
        displayTime = start + ms;
        cleanCanvas();
        display.drawCo2Setting();
        if (ms >= 40 || before > 3000 || next > 3000)
          sameRegion(raw, {25, 31, 77, 17}, "CO2 10Hz final raster");
        else {
          for (int slot = 0; slot < 4; slot++)
            if (oldDigits[slot] == newDigits[slot])
              sameRegion(raw, {28 + slot * 11, 32, 11, 15}, "CO2 unchanged digit");
          sameRegion(raw, {72, 31, 30, 17}, "CO2 stationary unit");
        }
        for (int y : {31, 47})
          for (int x = 25; x <= 101; x++) assert(!u8g2.lit(x, y));
        if (before == 1000 && next == 1050) {
          if (ms == 0) save("glyph-roll-start");
          if (ms == 16) save("glyph-roll-middle");
          if (ms == 48) save("glyph-roll-end");
        }
        frames++;
      }
    }
  printf(
      "Glyph integrity: %u actual CO2 transition frames at 10Hz preserve stationary digits/ppm and "
      "settle fully.\n",
      frames);
}
static void keyboardMotionIntegrity() {
  const char* sets[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789",
                        "!@#$%^&*()_+-=[]{}|;':\",./<>?"};
  const char* commands[] = {"DEL", "OK", "CLR", "ESC", "CUR"};
  unsigned frames = 0;
  for (int mode = 0; mode < 5; mode++) {
    textInputMgr.reset("", 64);
    for (int m = 0; m < mode; m++) textMode();
    int count = mode == 4 ? 5 : strlen(sets[mode]), index = 0;
    for (int dir : {1, -1})
      for (int step = 0; step < count; step++) {
        textStep(dir);
        index = (index + dir + count) % count;
        uint32_t start = displayTime;
        for (int ms = 0; ms <= 80; ms += 8) {
          displayTime = start + ms;
          cleanCanvas();
          textInputMgr.draw();
          for (int y = 33; y <= 56; y++) assert(u8g2.lit(54, y) && u8g2.lit(74, y));
          for (int y = 35; y <= 54; y++)
            for (int x : {53, 55, 73, 75}) assert(!u8g2.lit(x, y));
          for (int y : {34, 55})
            for (int x = 55; x <= 73; x++) assert(!u8g2.lit(x, y));
          if (ms == 80) {
            uint8_t actual[1024];
            memcpy(actual, u8g2.bytes, 1024);
            char letter[] = {mode == 4 ? ' ' : sets[mode][index], 0};
            u8g2.setFont(mode == 4 ? u8g2_font_6x10_tr : u8g2_font_ncenB12_tr);
            rawCentered(mode == 4 ? commands[index] : letter, {54, 33, 21, 24},
                        mode == 4 ? 49 : 51);
            sameRegion(actual, {55, 34, 19, 22}, "keyboard settled after movement");
            memcpy(u8g2.bytes, actual, 1024);
          }
          if (mode == 4 && dir == 1 && step == 0 && ms == 80) save("glyph-keyboard-cmd");
          if (mode == 1 && dir == 1 && step == 22 && ms == 24) save("glyph-keyboard-crossing");
          frames++;
        }
      }
    // Reverse repeatedly before the preceding move completes.
    for (int step = 0; step < 20; step++) {
      textStep(step % 2 ? -1 : 1);
      cleanCanvas();
      textInputMgr.draw();
      for (int y = 35; y <= 54; y++)
        for (int x : {53, 55, 73, 75}) assert(!u8g2.lit(x, y));
    }
  }
  printf(
      "Glyph integrity: %u bidirectional keyboard frames and 100 interrupted reversals preserve "
      "wall clearance.\n",
      frames);
}
static void commonAlignmentIntegrity() {
  struct Label {
    const char* text;
    oled::Rect box;
    const uint8_t* font;
    int baseline;
  };
  Label labels[] = {{"No", {16, 51, 34, 12}, u8g2_font_ncenB08_tr, 61},
                    {"Yes", {74, 51, 38, 12}, u8g2_font_ncenB08_tr, 61},
                    {"On", {10, 19, 34, 11}, u8g2_font_6x10_tr, 28},
                    {"Off", {10, 19, 34, 11}, u8g2_font_6x10_tr, 28},
                    {"Cancel", {2, 50, 40, 11}, u8g2_font_6x10_tr, 59},
                    {"Save", {46, 50, 32, 11}, u8g2_font_6x10_tr, 59},
                    {"Delete", {82, 50, 44, 11}, u8g2_font_6x10_tr, 59},
                    {"More >", {86, 35, 40, 11}, u8g2_font_6x10_tr, 44},
                    {"Back", {10, 51, 42, 11}, u8g2_font_6x10_tr, 60},
                    {"Learn", {74, 51, 48, 11}, u8g2_font_6x10_tr, 60},
                    {"Wait", {74, 51, 48, 11}, u8g2_font_6x10_tr, 60}};
  for (const auto& label : labels) {
    u8g2.setFont(label.font);
    rawCentered(label.text, label.box, label.baseline);
    auto ink = inkBounds();
    auto box = label.box;
    assert(ink.x >= box.x + 2 && ink.x + ink.w <= box.x + box.w - 2);
    assert(ink.y >= box.y + 2 && ink.y + ink.h <= box.y + box.h - 2);
    uint8_t raw[1024];
    memcpy(raw, u8g2.bytes, 1024);
    cleanCanvas();
    oled::centeredText(u8g2, box, label.baseline, label.text);
    sameRegion(raw, {0, 0, 128, 64}, label.text);
    for (float fill : {0.f, .5f, 1.f}) {
      memcpy(u8g2.bytes, raw, 1024);
      oled::selection(u8g2, box, false, fill);
      for (int y = box.y + 1; y < box.y + box.h - 1; y++)
        for (int x = box.x + 1; x < box.x + box.w - 1; x++) {
          bool inverted = x < box.x + 1 + oled::pixel((box.w - 2) * fill);
          assert(u8g2.lit(x, y) == (pixelIn(raw, x, y) != inverted));
        }
    }
  }
  puts(
      "Glyph integrity: 11 shared button labels match independently centered ink, retain padding "
      "and fill without a gap.");
}
static void settingBracketChecks() {
  bool savedSettle = settleFocus;
  settleFocus = true;
  struct Row {
    UIState page;
    int item;
    const char* name;
    const char* value;
  };
  Row rows[] = {
      {MOTION_SETTING, 1, "Speed", "18.0"},      {MOTION_SETTING, 2, "Curve", "100%"},
      {MOTION_SETTING, 3, "Coverage", "100%"},   {MOTION_SETTING, 4, "Info Cycle", "10s"},
      {MOTION_SETTING, 5, "Motion", "Organic"},  {MOTION_SETTING, 6, "Jerk Limit", "100%"},
      {INPUT_SETTING, 1, "Debounce", "50ms"},    {INPUT_SETTING, 2, "Hold Delay", "3000ms"},
      {INPUT_SETTING, 3, "Repeat Wait", "1000ms"},
      {INPUT_SETTING, 4, "Repeat Rate", "10/s"}, {INPUT_SETTING, 5, "Chord", "120ms"},
      {ALARM_OPTIONS, 2, "Wake Check", "10m"},   {ALARM_OPTIONS, 3, "Check Limit", "180s"},
      {ALARM_SNOOZE, 1, "Interval", "20 min"},   {ALARM_SNOOZE, 2, "Max Times", "5"}};
  auto savedMotion = motionSettings;
  auto savedInput = inputSettings;
  auto savedAlarm = state.tempAlarmOptions;
  motionSettings.speedHalf = 36;
  motionSettings.curve = motionSettings.coverage = motionSettings.jerk = 100;
  motionSettings.cycleSeconds = 10;
  motionSettings.organic = true;
  inputSettings.debounceMs = 50;
  inputSettings.holdDelayMs = 3000;
  inputSettings.repeatDelayMs = 1000;
  inputSettings.repeatHz = 10;
  inputSettings.chordMs = 120;
  state.tempAlarmOptions.checkMinutes = 10;
  state.tempAlarmOptions.checkTimeoutSec = 180;
  state.tempAlarmOptions.snoozeMinutes = 20;
  state.tempAlarmOptions.snoozeCount = 5;
  for (const auto& row : rows)
    for (bool editing : {false, true}) {
      state.uiState = row.page;
      state.isEditing = editing;
      state.motionIdx = state.inputIdx = state.optionIdx = state.snoozeIdx = row.item;
      int offset =
          (row.page == MOTION_SETTING || row.page == INPUT_SETTING) ? max(0, row.item - 4) : 0;
      state.motionOffset = state.inputOffset = state.optionOffset = offset;
      display.scrollView = -1;
      display.rolling.reset();
      cleanCanvas();
      if (row.page == MOTION_SETTING) display.drawMotionSettings();
      if (row.page == INPUT_SETTING) display.drawInputSettings();
      if (row.page == ALARM_OPTIONS) display.drawAlarmOptions();
      if (row.page == ALARM_SNOOZE) display.drawSnooze();
      display.verify("bracketed maximum row");
      uint8_t actual[1024];
      memcpy(actual, u8g2.bytes, 1024);
      cleanCanvas();
      u8g2.setFont(u8g2_font_6x10_tr);
      char expected[48];
      snprintf(expected, sizeof(expected), "%s [%s]", row.name, row.value);
      int baseline = 12 + (row.item - offset) * 12;
      u8g2.drawStr(4, baseline, expected);
      assert(inkBounds().x + inkBounds().w <= 126);
      sameRegion(actual, {2, baseline - 7, 124, 9}, "brackets, value and units");
      memcpy(u8g2.bytes, actual, 1024);
      display.finish();
      if (row.page == MOTION_SETTING && row.item == 6 && !editing) save("glyph-motion");
      if (row.page == INPUT_SETTING && row.item == 5 && !editing) save("glyph-input");
      if (row.page == ALARM_OPTIONS && !editing) save("glyph-alarm-options");
      if (row.page == ALARM_SNOOZE && row.item == 2 && !editing) save("glyph-snooze");
    }
  motionSettings = savedMotion;
  inputSettings = savedInput;
  state.tempAlarmOptions = savedAlarm;
  state.isEditing = false;
  settleFocus = savedSettle;
  printf("Glyph integrity: all %u editable list rows retain brackets and fit their maximum values.\n",
         static_cast<unsigned>(sizeof(rows)/sizeof(rows[0])));
}
static void bracketAnimationIntegrity() {
  struct Change {
    const char* name;
    const char* from;
    const char* to;
  };
  Change changes[] = {{"Hold Delay", " 950ms", "1000ms"},
                      {"Speed", " 9.5", "10.0"},
                      {"Coverage", " 95%", "100%"},
                      {"Repeat Rate", " 9/s", "10/s"},
                      {"Wake Check", " 9m", "10m"}};
  state.uiState = INPUT_SETTING;
  state.isEditing = true;
  state.valueDirection = 1;
  for (auto c : changes) {
    displayTime = 50000;
    display.rolling.reset();
    cleanCanvas();
    display.drawSettingRow(1, 1, 0, c.name, c.from, true);
    for (int ms : {0, 16, 32, 48, 64}) {
      displayTime = 50100 + ms;
      cleanCanvas();
      display.drawSettingRow(1, 1, 0, c.name, c.to, true);
      uint8_t actual[1024];
      memcpy(actual, u8g2.bytes, 1024);
      cleanCanvas();
      u8g2.setFont(u8g2_font_6x10_tr);
      char label[48];
      snprintf(label, sizeof(label), "%s [%s]", c.name, c.to);
      u8g2.drawStr(4, 24, label);
      int start = 4 + (strlen(c.name) + 2) * 6;
      for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++) {
          int slot = (x - start) / 6;
          bool changed =
              x >= start && slot < static_cast<int>(strlen(c.to)) && c.from[slot] != c.to[slot];
          if (ms >= 40 || !changed) assert(u8g2.lit(x, y) == pixelIn(actual, x, y));
        }
    }
  }
  state.isEditing = false;
  display.queued = false;
  puts(
      "Glyph integrity: list brackets, labels, unchanged digits and units stay fixed across 25 "
      "animation frames.");
}
