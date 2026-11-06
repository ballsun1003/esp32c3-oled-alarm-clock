// Included after the actual firmware drawing/input classes and U8g2 canvas.
static void textTick(bool left, bool right, bool ok = false, int dt = 10) {
  displayTime += dt;
  btnLeft.pressed = left;
  btnRight.pressed = right;
  textInputMgr.processInput(ok);
}
static void textStep(int direction) {
  textTick(direction<0, direction> 0);
  textTick(false, false);
}
static void textMode() {
  textTick(true, false);
  textTick(true, true);
  textTick(false, true);
  textTick(false, false);
}
static void setupChecks() {
  inputSettings=oled::InputSettings{};
  state.uiState = INPUT_SETTING;
  state.isEditing = false;
  state.inputIdx = INPUT_ITEMS - 1;
  inputSettings.debounceMs = 50;
  motionSettings.jerk = 25;
  setupInput(false, false, true, true, 100);
  setupInput(false, false, false, false, 1099);
  assert(inputSettings.debounceMs == 50 && !uiHold.running());
  setupInput(false, false, true, true, 2000);
  state.inputOffset=INPUT_ITEMS-5;displayTime=2500;
  u8g2.clear();display.drawInputSettings();display.finish();save("hold-half");
  setupInput(false, false, false, true, 3300);
  assert(inputSettings.debounceMs == 50);  // Elapsed alone cannot trigger it.
  displayTime = 3300;
  u8g2.clear();
  display.drawInputSettings();
  display.finish();
  save("hold-full");
  setupInput(false, false, false, true, 3301);
  assert(inputSettings.debounceMs == 5 && motionSettings.jerk == 25 && inputSaves == 1);
  setupInput(false, false, false, true, 5000);
  assert(inputSaves == 1);
  state.uiState = MOTION_SETTING;
  state.motionIdx = MOTION_ITEMS - 1;
  inputSettings.debounceMs = 20;
  setupInput(false, false, true, true, 6000);
  displayTime = 7200;
  u8g2.clear();
  display.drawMotionSettings();
  display.finish();
  setupInput(false, false, false, true, 7201);
  assert(motionSettings.jerk == 60 && inputSettings.debounceMs == 20 && motionSaves == 1);
  for (UIState screen : {INPUT_SETTING, MOTION_SETTING}) {
    state.uiState = screen;
    state.isEditing = false;
    state.inputIdx = state.motionIdx = 0;
    for (int i = 0; i < 30; i++) setupInput(false, true, false, false, 8000 + i);
    assert((screen == INPUT_SETTING ? state.inputIdx : state.motionIdx) ==
           (screen == INPUT_SETTING ? INPUT_ITEMS : MOTION_ITEMS) - 1);
    for (int i = 0; i < 30; i++) setupInput(true, false, false, false, 9000 + i);
    assert((screen == INPUT_SETTING ? state.inputIdx : state.motionIdx) == 0);
    setupInput(false, false, true, true, 10000);
    assert(state.uiState == MAIN_MENU);
  }
  inputSettings = oled::InputSettings{};
  motionSettings = oled::MotionSettings{};
}
static void newUiChecks() {
  uint8_t sent[1024] = {};
  bool valid = false;
  u8g2.clear();
  oled::presentChanged(u8g2, sent, valid);
  assert(u8g2.transferred == 1024);
  oled::presentChanged(u8g2, sent, valid);
  assert(u8g2.transferred == 1024);
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 128; x += 7) {
      u8g2.drawPixel(x, y);
      oled::presentChanged(u8g2, sent, valid);
      assert(memcmp(u8g2.panel, u8g2.bytes, 1024) == 0);
    }
  u8g2.clear();
  oled::presentChanged(u8g2, sent, valid);
  assert(memcmp(u8g2.panel, u8g2.bytes, 1024) == 0);
  unsigned before = u8g2.transferred;
  u8g2.drawPixel(127, 63);
  oled::presentChanged(u8g2, sent, valid);
  assert(u8g2.transferred - before == 8);
  state.isEditing = false;
  apMgr.apList.push_back({"< Back", "", -100, false, -3, false});
  for (int i = 0; i < 6; i++)
    apMgr.apList.push_back(
        {"Gypsy jumping WiFi long SSID", "aa:bb:cc:dd:ee:ff", -100 + i * 10, true, 1, bool(i % 2)});
  for (int i = 0; i < 6; i++) locResults.push_back({"Gypsy City", "Country", "", ""});
  for (int i = 0; i < 7; i++) {
    state.listIdx = i;
    state.listOffset = max(0, i - 4);
    state.uiState = WIFI_LIST;
    u8g2.clear();
    display.drawWiFiList();
    display.verify("WiFi");
    display.finish();
    if (i == 4) save("wifi-list");
    u8g2.clear();
    display.drawLocationResultList();
    display.verify("cities");
    display.finish();
    if (i < 6) {
      u8g2.clear();
      display.drawAlarmList();
      display.verify("alarm list");
      display.finish();
    }
  }
  setupChecks();
  const char* sets[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789",
                        "!@#$%^&*()_+-=[]{}|;':\",./<>?"};
  for (int mode = 0; mode < 4; mode++) {
    textInputMgr.reset("", 64);
    for (int i = 0; i < mode; i++) textMode();
    std::string expected;
    for (const char* p = sets[mode]; *p; p++) {
      textTick(false, false, true);
      expected += *p;
      assert(textInputMgr.getInputString() == expected);
      for (int dt : {0, 16, 32, 64, 80}) {
        u8g2.clear();
        displayTime += dt;
        textInputMgr.draw();
        // A fixed, unbroken frame throughout the font transition.
        for (int y = 33; y < 57; y++) assert(u8g2.lit(54, y) && u8g2.lit(74, y));
      }
      textStep(1);
    }
    textTick(false, false, true);
    expected += sets[mode][0];
    assert(textInputMgr.getInputString() == expected);  // Full wrap, including symbols.
    textStep(-1);
    textTick(false, false, true);
    expected += sets[mode][strlen(sets[mode]) - 1];
    assert(textInputMgr.getInputString() == expected);
  }
  textInputMgr.reset("", 64);
  for (int i = 0; i < 70; i++) textTick(false, false, true);
  assert(textInputMgr.getInputString().length() == 64);
  textInputMgr.reset("abc", 64);
  for (int i = 0; i < 4; i++) textMode();
  textTick(false, false, true);
  assert(textInputMgr.getInputString() == "ab");  // DEL
  textStep(-1);
  textTick(false, false, true);  // CUR
  textStep(-1);
  textTick(false, false, true);  // Move cursor between a,b and exit.
  textMode();
  textTick(false, false, true);
  assert(textInputMgr.getInputString() == "aab");
  for (int i = 0; i < 4; i++) textMode();  // Remembered CMD selection: CUR.
  textStep(-1);
  assert(textInputMgr.processInput(true) == 2);  // ESC
  textStep(-1);
  textTick(false, false, true);
  assert(textInputMgr.getInputString().empty());  // CLR
  textStep(-1);
  assert(textInputMgr.processInput(true) == 1);  // OK
  textInputMgr.reset("Sample WiFi", 64);
  textMode();
  for (int i = 0; i < 22; i++) textStep(1);  // Large W and two font sizes at box walls.
  displayTime += 100;
  u8g2.clear();
  textInputMgr.draw();
  save("carousel-rest");
  textStep(1);
  uint32_t start = displayTime;
  for (int ms : {0, 16, 32, 48, 80}) {
    displayTime = start + ms;
    u8g2.clear();
    textInputMgr.draw();
    char name[32];
    snprintf(name, sizeof(name), "carousel-%d", ms);
    save(name);
  }
  state.uiState = CO2_SETTING;
  state.isEditing = true;
  for (int value : {950, 1000, 3000, 3050, 500, 550, 500}) {
    state.co2Limit = value;
    displayTime += 100;
    uint32_t start = displayTime;
    for (int ms : {0, 20, 40, 60}) {
      displayTime = start + ms;
      u8g2.clear();
      display.drawCo2Setting();
      assert(!display.queued); // Decorative frame; glyph integrity is checked independently.
      display.finish();
      if (value == 500) {
        char name[32];
        snprintf(name, sizeof(name), "rolling-%d", ms);
        save(name);
      }
    }
  }
  // Every 100ms change must be fully settled by 60ms, including reversals.
  oled::RollingText roll;
  for (int n = 0; n < 100; n++) {
    char value[8];
    snprintf(value, sizeof(value), "%02d", n % 60);
    uint32_t now = 0xfffff000U + n * 100;
    u8g2.clear();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    roll.draw(u8g2, 1, value, 50, 30, {45, 18, 40, 14}, n % 2 ? 1 : -1, now, true);
    u8g2.clear();
    roll.draw(u8g2, 1, value, 50, 30, {45, 18, 40, 14}, 1, now + 60, true);
    uint8_t actual[1024];
    memcpy(actual, u8g2.bytes, 1024);
    u8g2.clear();
    u8g2.drawStr(50, 30, value);
    assert(memcmp(actual, u8g2.bytes, 1024) == 0);
  }
  // Full-screen loading always uses the original spinner anchor, independent of text.
  for (const char* message : {"Finding AP...", "Disconnected!\nFinding AP..."}) {
    for (int frame = 0; frame < 8; frame++) {
      displayTime = frame * 60;
      u8g2.clear();
      display.drawLoading(message, message[0]=='F'?34:40, true);
      uint8_t actual[1024];memcpy(actual,u8g2.bytes,1024);
      u8g2.clear();display.drawSpinner(64,58);
      for(int y=50;y<64;y++)for(int x=0;x<128;x++)
        assert(bool(actual[(y>>3)*128+x]&(1<<(y&7)))==u8g2.lit(x,y));
      memcpy(u8g2.bytes,actual,1024);
    }
    save(message[0] == 'F' ? "finding-ap" : "disconnected-ap");
  }
  oled::Mask weather[4], clock;
  display.rasterClock(clock);
  for (int cycle = 0; cycle < 4; cycle++) {
    u8g2.clear();
    display.drawCO2(8, 24, 1234);
    display.drawWeatherInfoRow(8, 41, cycle);
    assert(weather[cycle].capture(u8g2.bytes));
    assert(weather[cycle].h <= 32);
    char name[32];
    snprintf(name, sizeof(name), "weather-%d", cycle);
    save(name);
  }
  oled::Motion moving;
  moving.initialize(clock, weather[0]);
  moving.settings.jerk = 100;
  int phase = 0;
  bool offered = false;
  unsigned maxWait = 0, wait = 0;
  const bool captureDemo = getenv("CO2_CAPTURE_DEMO") != nullptr;
  for (int frame = 0; frame < 30 * 600; frame++) {
    if (!offered && frame % 90 == 0) {
      phase = (phase + 1) % 4;
      moving.offer(1, weather[phase]);
      offered = true;
    }
    moving.advance(1 / 30.f);
    if (captureDemo && frame < 360 && frame % 3 == 0) {
      u8g2.clear();
      for (const auto& body : moving.bodies) {
        auto r = body.box();
        for (int y = 0; y < r.h; ++y)
          for (int x = 0; x < r.w; ++x)
            if (body.mask.lit(x, y)) u8g2.drawPixel(r.x + x, r.y + y);
      }
      char name[24];
      snprintf(name, sizeof(name), "demo-%03d", frame / 3);
      save(name);
    }
    for (auto& body : moving.bodies) assert(oled::inside(body.box()));
    assert(!oled::intersects(moving.bodies[0].box(), moving.bodies[1].box()));
    if (offered) {
      if (moving.bodies[1].pending)
        wait++;
      else {
        maxWait = max(maxWait, wait);
        wait = 0;
        offered = false;
      }
    }
  }
  assert(maxWait < 300 && wait < 300);
  printf(
      "Current clock/weather glyphs including degree and PM: 10min collision/growth run passed, "
      "maximum wait %.2fs.\n",
      maxWait / 30.f);
  puts(
      "New UI: actual input charsets/commands/64-char capacity, setup hold/reset isolation, 10Hz "
      "rolling, menus and weather rasters passed.");
}
