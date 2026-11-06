// Opt-in recordings. Feed button events to the firmware's menu cases and save
// the final composited U8g2 buffer, plus the physical buttons for the caption.
namespace readme {
enum Button : uint8_t { Left = 1, Right = 2, OK = 4 };
constexpr int frameMs = 40;

class Clip {
  FILE* output;
  oled::DebouncedButton left, right, ok;
  bool navigationChordActive = false, alarmListActionExecuted = false;
  uint8_t alarmListPendingBtn = 0;
  unsigned long alarmListFirstPressTime = 0;

  void input(uint8_t mask) {
    const unsigned long now = millis();
    bool u = left.update(mask & Left, now, inputSettings, true);
    bool d = right.update(mask & Right, now, inputSettings, true);
    bool e = ok.update(mask & OK, now, inputSettings, false);
    bool leftPressed = left.down(), rightPressed = right.down(), okPressed = ok.down();
    btnLeft.pressed = leftPressed;
    btnRight.pressed = rightPressed;
    bool bothHeld = leftPressed && rightPressed;
    if (u || d) state.valueDirection = d ? 1 : -1;
    if (u || d || e || bothHeld) state.lastInputTime = now;
    if (navigationChordActive) {
      if (!leftPressed && !rightPressed) navigationChordActive = false;
      return;
    }
    if (fullResetCalls) return;
    const int previous = uiPage(state.uiState);
    switch (state.uiState) {
#include "../_backup/demo-menu-input.h"
      case LOCATION_SEARCH_INPUT: textInputMgr.processInput(e); break;
      default: setupInput(u, d, e, okPressed, now); break;
    }
    if (e && previous != uiPage(state.uiState)) display.animation.activate();
  }

  void frame(uint8_t mask) {
    const UIState before = state.uiState;
    input(mask);
    if (before != state.uiState || (!state.isEditing && state.uiState != CO2_SETTING))
      display.rolling.reset();
    display.animation.begin(fullResetCalls ? 101 : uiPage(state.uiState), display.uiDepth(),
                            millis(), !fullResetCalls);
    u8g2.setMaxClipWindow();
    u8g2.clear();
    display.queued = false;
    if (fullResetCalls) {
      display.drawLoading("Resetting...\nPlease wait", 40, true);
    } else {
      switch (state.uiState) {
        case MAIN_MENU: display.drawMainMenu(); break;
        case ALARM_LIST: display.drawAlarmList(); break;
        case ALARM_EDIT: display.drawAlarmEdit(); break;
        case ALARM_DAY_SELECT: display.drawDaySelect(); break;
        case ALARM_OPTIONS: display.drawAlarmOptions(); break;
        case INPUT_SETTING: display.drawInputSettings(); break;
        case RESET_CONFIRM: display.drawResetConfirm(); break;
        case CO2_SETTING: display.drawCo2Setting(); break;
        case ABC_CONFIRM: display.drawAbcConfirm(); break;
        case LOCATION_SEARCH_INPUT: textInputMgr.draw(); break;
        default: assert(false);
      }
      display.finish();
      display.drawMenuCloseWarning();
    }
    display.animation.compose(u8g2.bytes, millis());
    assert(fputc(mask, output) != EOF);
    assert(fwrite(u8g2.bytes, 1, sizeof(u8g2.bytes), output) == sizeof(u8g2.bytes));
    displayTime += frameMs;
  }

 public:
  Clip(const char* name, UIState page) {
    std::string path = std::string("_backup/") + name + ".frames";
    output = fopen(path.c_str(), "wb");
    assert(output);
    state = {};
    state.uiState = page;
    state.tempAlarm = {7, 30, 0, true};
    state.tempAlarmOptions = AlarmOptions{};
    state.lastInputTime = displayTime = 400000;
    inputSettings = oled::InputSettings{};
    uiHold.cancel();
    settleFocus = false;
    display.animation = oled::UiAnimation{};
    display.scrollView = -1;
    display.rolling.reset();
    fullResetCalls = 0;
  }
  ~Clip() { assert(fclose(output) == 0); }
  void show(int ms, uint8_t buttons = 0) {
    assert(ms % frameMs == 0);
    for (int elapsed = 0; elapsed < ms; elapsed += frameMs) frame(buttons);
  }
  void tap(uint8_t buttons, int pause = 520) {
    show(200, buttons);
    show(pause);
  }
};
}  // namespace readme

void captureReadmeInteractions() {
  using namespace readme;
  {
    Clip clip("alarm-shortcut", MAIN_MENU);
    alarmMgr.alarmCount = 2;
    alarmMgr.alarms[0] = {7, 30, 127, false};
    alarmMgr.alarms[1] = {9, 0, 0, true};
    clip.show(880);
    clip.tap(Right);
    clip.tap(OK, 720);
    clip.tap(Right);
    clip.tap(Left | Right, 1280);
    assert(state.uiState == ALARM_LIST && state.listIdx == 1 && alarmMgr.alarms[0].enabled);
  }
  {
    Clip clip("focus-and-days", ALARM_EDIT);
    state.editRow = 2;
    clip.show(880);
    clip.tap(OK);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right, 1440);
    assert(state.tempAlarm.minute == 31 && state.tempAlarm.days == 2 && state.daySelectIdx == 2);
  }
  {
    Clip clip("sensor-setup", MAIN_MENU);
    state.menuIdx = 2;
    state.co2Limit = 1200;
    clip.show(960);
    clip.tap(OK, 1280);
    clip.tap(OK);
    clip.tap(Right, 680);
    clip.tap(OK, 1440);
    assert(state.uiState == ABC_CONFIRM && !state.abcConfirmYes);
  }
  {
    Clip clip("inline-setting", INPUT_SETTING);
    state.inputIdx = 1;
    clip.show(720);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right, 880);
    clip.tap(OK, 960);
    assert(inputSettings.holdDelayMs == 1050 && !state.isEditing);
  }
  {
    Clip clip("alarm-more", ALARM_EDIT);
    state.editRow = 3;
    clip.show(880);
    clip.tap(Right);
    clip.tap(OK, 880);
    clip.tap(Right);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right);
    clip.tap(OK);
    clip.tap(Right, 1120);
    assert(state.optionIdx == 3 && state.tempAlarmOptions.checkMinutes == 1);
  }
  {
    Clip clip("hold-to-reset", RESET_CONFIRM);
    clip.show(960);
    clip.tap(Right);
    clip.show(1240, OK);
    clip.show(800);
    assert(!uiHold.running() && state.resetConfirmStep == 0);
    clip.show(3200, OK);
    clip.show(880);
    assert(state.resetConfirmStep == 1 && !state.resetConfirmYes && !fullResetCalls);
    clip.tap(Right);
    clip.show(3200, OK);
    clip.show(960);
    assert(fullResetCalls == 1);
  }
  {
    Clip clip("city-keyboard", LOCATION_SEARCH_INPUT);
    textInputMgr.reset("Zagre");
    clip.show(960);
    clip.tap(Right, 840);
    clip.tap(OK, 1440);
    assert(textInputMgr.getInputString() == "Zagreb");
  }
  {
    Clip clip("menu-warning", MAIN_MENU);
    state.menuIdx = 1;
    state.lastInputTime -= MENU_INACTIVITY_TIMEOUT_MS - MENU_INACTIVITY_WARNING_MS - 800;
    clip.show(3240);
    clip.tap(Right, 1280);
    assert(state.uiState == MAIN_MENU && state.menuIdx == 2);
    assert(millis() - state.lastInputTime < MENU_INACTIVITY_WARNING_MS);
  }
}
