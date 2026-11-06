#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESP32Time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <atomic>
#include <vector>
#include <algorithm>
#include <math.h>
#include <esp_system.h>
#include <esp_sntp.h>
#include "oled_motion.h"
#include "oled_ui.h"
#include "oled_input.h"
#include "oled_widgets.h"

// =================================================================================
// 1. Configuration & Constants
// =================================================================================

#define PIN_SDA 4
#define PIN_SCL 5
#define PIN_RX 7
#define PIN_TX 6
#define PIN_BUZZER 8
#define PIN_CDS 0
#define PIN_BTN_UP 1
#define PIN_BTN_DOWN 3
#define PIN_BTN_OK 10

#define MAX_ALARMS 100
#define MAX_SAVED_WIFI 100
#define MAX_LOCATION_RESULTS 20
#define SCREEN_W 128
#define SCREEN_H 64
#define DARK_THRESHOLD 3800
#define SCREEN_OFF_TIMEOUT 60000
#define TARGET_FPS 60
#define FRAME_DELAY (1000 / TARGET_FPS)
#define WEATHER_INFO_COUNT 4
#define WEATHER_INFO_PERIOD_MS 3000
#define MENU_INACTIVITY_TIMEOUT_MS 300000UL
#define MENU_INACTIVITY_WARNING_MS 30000UL
#define ALARM_STATIC_DISPLAY_MS 60000UL
#define HTTP_TIMEOUT 5000
#define MAX_SYNC_RETRY 3
#define WEATHER_EXPIRE_MS 3600000
#define WEATHER_RETRY_MS  120000
#define TIME_RETRY_MS 2000
#define SENSOR_READ_PERIOD 2000
#define WEATHER_MIN_REQUEST_GAP_MS 15000
#define ALARM_DISMISS_GUARD_MS 750
#define DUAL_BUTTON_GRACE_MS inputSettings.chordMs
#define CO2_TONE_FREQUENCY_HZ 2500
#define CO2_TONE_DURATION_MS 200
#define TIME_ALARM_PERIOD_MS 1000
#define TIME_ALARM_ON_MS 500
#define ALARM_OUTPUT_TICK_MS 10
#define ALARM_OUTPUT_TASK_STACK_SIZE 4096
#define NETWORK_TASK_STACK_SIZE 12288
#define NETWORK_ENQUEUE_TIMEOUT_MS 1
#define SENSOR_TASK_STACK_SIZE 4096
#define WIFI_SCAN_TIMEOUT_MS 12000
#define STARTUP_CONNECT_WAIT_MS 10000
#define STARTUP_SCAN_PREP_MS 500
#define STARTUP_SCAN_WAIT_MS 8000
#define STARTUP_FOUND_AP_INFO_MS 1500
#define STARTUP_RECONNECT_WAIT_MS 30000
#define STARTUP_SYNC_TIMEOUT_MS 30000
#define FULL_RESET_HOLD_MS 3000
#define ALARM_LOOKBACK_MINUTES 60
enum UIState {
  SCREEN_SAVER, MAIN_MENU, ALARM_LIST, ALARM_EDIT, ALARM_DAY_SELECT,
  CO2_SETTING, LOCATION_SETUP, CALIBRATION_CONFIRM, ABC_CONFIRM, WIFI_SCANNING,
  WIFI_LIST, WIFI_PASS_INPUT, WIFI_CONNECTING, LOCATION_SEARCH_INPUT,
  LOCATION_RESULT_LIST,
  RESET_CONFIRM, MOTION_SETTING, ALARM_OPTIONS, ALARM_SNOOZE, ALARM_LIGHT, INPUT_SETTING
};

enum BuzzerMode {
  BUZZER_SILENT,
  BUZZER_CO2_ALERT,
  BUZZER_TIME_ALARM
};

struct Alarm {
  uint8_t hour, minute, days;
  bool enabled;
};

struct SavedWiFi {
  char ssid[33];
  char pass[65];
  uint32_t lastUsedTick;
};

struct APItem {
  String ssid;
  String bssid;
  int rssi;
  bool isSecured;
  int category;
  bool hasDuplicate;
};

struct LocationResult {
  String name, country, lat, lon;
};

struct WeatherSnapshot {
  long utcOffsetSeconds = 0;
  float tempOut = 0.0f;
  int weatherCode = 0;
  int isDay = 1;
  int rainProb = 0;
  float pm10 = 0.0f;
  float windSpeed = 0.0f;
  int windDir = 0;
};

struct PackedLocationResult {
  char name[48];
  char country[48];
  char lat[24];
  char lon[24];
};

enum NetworkJobType {
  NETWORK_JOB_NONE,
  NETWORK_JOB_WEATHER_SYNC,
  NETWORK_JOB_LOCATION_SEARCH
};

enum NetworkJobContext {
  NETWORK_CONTEXT_NONE,
  NETWORK_CONTEXT_STARTUP,
  NETWORK_CONTEXT_BACKGROUND,
  NETWORK_CONTEXT_LOCATION
};

// Settings written by this firmware. Upload with cleared NVS; no legacy migration.
struct AlarmOptions {
  uint8_t snoozeMinutes = 5, snoozeCount = 0, checkMinutes = 0, mission = 0;
  uint16_t lightRaw = 0xffff;
  uint8_t checkTimeoutSec = 60;
};

enum StartupPhase {
  STARTUP_IDLE,
  STARTUP_NO_WIFI_INFO,
  STARTUP_WAIT_INITIAL_CONNECT,
  STARTUP_PREPARE_SCAN,
  STARTUP_WAIT_SCAN,
  STARTUP_FOUND_AP_INFO,
  STARTUP_WAIT_RECONNECT,
  STARTUP_WAIT_SYNC
};

enum WiFiScanMessage {
  WIFI_SCAN_MESSAGE_NONE,
  WIFI_SCAN_MESSAGE_CONNECTION_FAILED,
  WIFI_SCAN_MESSAGE_DISCONNECTED
};
struct NetworkJob {
  NetworkJobType type = NETWORK_JOB_NONE;
  NetworkJobContext context = NETWORK_CONTEXT_NONE;
  char lat[24] = "";
  char lon[24] = "";
  char query[64] = "";
};

struct NetworkResult {
  NetworkJobType type = NETWORK_JOB_NONE;
  NetworkJobContext context = NETWORK_CONTEXT_NONE;
  bool success = false;
  bool hasWeatherData = false;
  bool hasLocationResults = false;
  char lat[24] = "";
  char lon[24] = "";
  long utcOffsetSeconds = 0;
  float tempOut = 0.0f;
  int weatherCode = 0;
  int isDay = 1;
  int rainProb = 0;
  float pm10 = 0.0f;
  float windSpeed = 0.0f;
  int windDir = 0;
  int locationCount = 0;
  PackedLocationResult locationResults[MAX_LOCATION_RESULTS] = {};
};

struct LoadingOverlayState {
  bool active = false;
  bool showSpinner = false;
  bool blocksInput = true;
  unsigned long expiresAt = 0;
  int y = 40;
  char message[96] = "";
};

struct StartupFlowState {
  StartupPhase phase = STARTUP_IDLE;
  unsigned long phaseStartTime = 0;
  int syncRetryCount = 0;
  bool waitingForWeather = false;
  char foundSSID[33] = "";
  char foundPass[65] = "";
};

struct SensorReading {
  int co2 = 0;
  bool valid = false;
};

// =================================================================================
// 2. Global State Management
// =================================================================================

struct State {
  UIState uiState = SCREEN_SAVER;
  bool isScreenOff = false;
  bool isAmbientDark = false;
  unsigned long lastInputTime = 0;
  bool isTimeSynced = false;
  bool timeResyncPending = false;
  bool isWeatherSynced = false;
  int weatherInfoCycle = 0;
  bool weatherTransitionPending = false;
  unsigned long lastWeatherInfoCycleChange = 0;
  unsigned long lastSuccessfulWeatherUpdate = 0;
  unsigned long lastWeatherAttemptTime = 0;
  unsigned long lastTimeSyncAttemptTime = 0;
  unsigned long lastWiFiReconnectAttemptTime = 0;
  int filteredCds = 0;
  int displayContrast = 255;
  bool wifiAutoReconnectEnabled = true;

  int co2 = 0;
  int co2Limit = 1500;
  bool abcEnabled = true;
  bool isSensorError = false;
  bool isCo2Muted = false;

  float tempOut = 0.0;
  int weatherCode = 0;
  int isDay = 1;
  int rainProb = 0;
  float pm10 = 0.0;
  float windSpeed = 0.0;
  int windDir = 0;
  char cityName[32] = "";
  String lat = "", lon = "";
  int lastWeatherUpdateMinute = -1;

  unsigned long alarmStartTime = 0;

  int menuIdx = 0, menuOffset = 0;
  int listIdx = 0, listOffset = 0;
  int wifiListReturnCategory = 0;
  bool wifiHoldingOk = false, wifiLongHandled = false;
  int editingAlarmIdx = -1;
  int editRow = 0;
  bool isEditing = false;
  int daySelectIdx = 0;
  bool calConfirmYes = false;
  bool abcConfirmYes = false;
  bool abcConfirmTargetOn = false;
  int locMenuIdx = 0;
  bool resetConfirmYes = false;
  int resetConfirmStep = 0;
  bool fullResetPending = false;
  int fullResetStage = 0;
  unsigned long fullResetStageStarted = 0;
  Alarm tempAlarm;
  AlarmOptions tempAlarmOptions;
  int optionIdx = 0, optionOffset = 0, snoozeIdx = 0, lightIdx = 0;
  int motionIdx = 0, motionOffset = 0;
  int inputIdx = 0, inputOffset = 0;
  int valueDirection = 1;
  bool lightLearning = false;
  unsigned long lightLearnStart = 0, lightLearnSample = 0;
  uint16_t lightSamples[40] = {};
  uint8_t lightSampleCount = 0;
} state;

oled::MotionSettings motionSettings;
oled::InputSettings inputSettings;
oled::HoldGesture uiHold;
constexpr int MOTION_ITEMS=8, INPUT_ITEMS=7, MAIN_ITEMS=10, ALARM_OPTION_ITEMS=6;
// Keep free-function definitions after the shared types: Arduino inserts its
// generated prototypes here, including those using NetworkJobContext/Result.
// Connection and scan status replace content within the same WiFi page.
int uiPage(UIState view) {
  return view==WIFI_SCANNING || view==WIFI_CONNECTING ? WIFI_LIST : view;
}

void cancelMenuHold() {
  uiHold.cancel();state.wifiHoldingOk=false;state.wifiLongHandled=false;
}

void beginMenuHold(int item,unsigned long now) {
  uiHold.begin(state.uiState*1000+item,now,inputSettings.holdDelayMs);
}

void saveInputSettings() {
  Preferences p;
  if(!p.begin("clock",false))return;
  p.putUInt("btnDebounce",inputSettings.debounceMs);
  p.putUInt("btnHold",inputSettings.holdDelayMs);
  p.putUInt("btnRepeatWait",inputSettings.repeatDelayMs);
  p.putUInt("btnRepeat",inputSettings.repeatHz);
  p.putUInt("btnChord",inputSettings.chordMs);
  p.end();
}

void saveMotionSettings() {
  Preferences motionPrefs;
  if(!motionPrefs.begin("clock",false))return;
  motionPrefs.putInt("moveSpeed",motionSettings.speedHalf);
  motionPrefs.putInt("moveCurve",motionSettings.curve);
  motionPrefs.putInt("moveCover",motionSettings.coverage);
  motionPrefs.putInt("moveCycle",motionSettings.cycleSeconds);
  motionPrefs.putBool("moveOrganic",motionSettings.organic);
  motionPrefs.putInt("moveJerk",motionSettings.jerk);
  motionPrefs.end();
}

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
HardwareSerial sensorSerial(1);
Preferences prefs;
ESP32Time rtc(0);
std::vector<LocationResult> locResults;
LoadingOverlayState loadingOverlay;
StartupFlowState startupFlow;
std::atomic_bool ntpTimeSyncCompleted{false};

void onNtpTimeSynchronized(struct timeval* timeValue) {
  (void)timeValue;
  ntpTimeSyncCompleted.store(true);
}

void beginNtpTimeSync(unsigned long now) {
  ntpTimeSyncCompleted.store(false);
  // Keep the system clock in UTC; ESP32Time applies the location offset.
  configTzTime("UTC0", "pool.ntp.org");
  state.timeResyncPending = true;
  state.lastTimeSyncAttemptTime = now;
}

bool applyCompletedNtpTime() {
  if (!ntpTimeSyncCompleted.exchange(false)) return false;

  time_t epoch;
  struct tm timeinfo;
  time(&epoch);
  localtime_r(&epoch, &timeinfo);
  if (timeinfo.tm_year <= 120) {
    ntpTimeSyncCompleted.store(true);
    return false;
  }

  rtc.setTimeStruct(timeinfo);
  state.isTimeSynced = true;
  state.timeResyncPending = false;
  return true;
}

void releaseLocationResults() {
  std::vector<LocationResult>().swap(locResults);
}

// Fixed-size copy helpers are used whenever data crosses task boundaries.
void copyToBuffer(char* dest, size_t destSize, const char* src) {
  if (!dest || destSize == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, destSize - 1);
  dest[destSize - 1] = '\0';
}

void copyToBuffer(char* dest, size_t destSize, const String& src) {
  copyToBuffer(dest, destSize, src.c_str());
}

// Loading overlays render at most two centered lines, so split once up front.
void splitTextOnNewline(const char* text, char* line1, size_t line1Size, char* line2, size_t line2Size) {
  if (!text) {
    if (line1Size > 0) line1[0] = '\0';
    if (line2Size > 0) line2[0] = '\0';
    return;
  }

  const char* newline = strchr(text, '\n');
  if (!newline) {
    copyToBuffer(line1, line1Size, text);
    if (line2Size > 0) line2[0] = '\0';
    return;
  }

  size_t firstLen = static_cast<size_t>(newline - text);
  if (line1Size > 0) {
    if (firstLen >= line1Size) firstLen = line1Size - 1;
    memcpy(line1, text, firstLen);
    line1[firstLen] = '\0';
  }
  copyToBuffer(line2, line2Size, newline + 1);
}

int clampValue(int value, int minValue, int maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return value;
}

void clampListState(int totalItems, int visibleRows = 5) {
  if (totalItems <= 0) {
    state.listIdx = 0;
    state.listOffset = 0;
    return;
  }

  state.listIdx = clampValue(state.listIdx, 0, totalItems - 1);
  int maxOffset = (totalItems > visibleRows) ? (totalItems - visibleRows) : 0;
  state.listOffset = clampValue(state.listOffset, 0, maxOffset);

  if (state.listIdx < state.listOffset) state.listOffset = state.listIdx;
  if (state.listIdx >= state.listOffset + visibleRows) {
    state.listOffset = state.listIdx - visibleRows + 1;
  }
}

bool isCo2AlertRequested() {
  return !state.isSensorError &&
         state.co2Limit <= 3000 &&
         state.co2 > state.co2Limit &&
         !state.isCo2Muted;
}

class AlarmOutputController {
private:
  std::atomic_bool timeAlarmActive{false};
  std::atomic_bool co2AlarmRequested{false};
  std::atomic<uint8_t> activeMode{BUZZER_SILENT};
  TaskHandle_t workerHandle = nullptr;
  bool ledcAttached = false;
  bool co2CycleStarted = false;
  unsigned long co2CycleStart = 0;
  unsigned long timeAlarmCycleStart = 0;

  static void taskEntry(void* parameter) {
    static_cast<AlarmOutputController*>(parameter)->taskLoop();
  }

  void forceGpioLow() {
    if (ledcAttached) {
      ledcWriteTone(PIN_BUZZER, 0);
      ledcDetach(PIN_BUZZER);
      ledcAttached = false;
    }
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
  }

  bool startCo2Tone() {
    forceGpioLow();
    if (!ledcAttach(PIN_BUZZER, CO2_TONE_FREQUENCY_HZ, 10)) return false;
    ledcAttached = true;
    if (ledcWriteTone(PIN_BUZZER, CO2_TONE_FREQUENCY_HZ) != 0) return true;
    forceGpioLow();
    return false;
  }

  void setActiveMode(BuzzerMode mode) {
    activeMode.store(static_cast<uint8_t>(mode), std::memory_order_release);
  }

  void service(unsigned long now) {
    if (timeAlarmActive.load(std::memory_order_acquire)) {
      co2CycleStarted = false;
      if (getActiveMode() != BUZZER_TIME_ALARM) {
        forceGpioLow();
        timeAlarmCycleStart = now;
        setActiveMode(BUZZER_TIME_ALARM);
      }
      bool timeAlarmOn = (now - timeAlarmCycleStart) % TIME_ALARM_PERIOD_MS < TIME_ALARM_ON_MS;
      digitalWrite(PIN_BUZZER, timeAlarmOn ? HIGH : LOW);
      return;
    }

    if (!co2AlarmRequested.load(std::memory_order_acquire)) {
      co2CycleStarted = false;
      if (getActiveMode() != BUZZER_SILENT || ledcAttached) forceGpioLow();
      else digitalWrite(PIN_BUZZER, LOW);
      setActiveMode(BUZZER_SILENT);
      return;
    }

    if (!co2CycleStarted) {
      co2CycleStarted = true;
      co2CycleStart = now;
    }
    unsigned long elapsed = now - co2CycleStart;
    if (elapsed >= SENSOR_READ_PERIOD) {
      co2CycleStart += (elapsed / SENSOR_READ_PERIOD) * SENSOR_READ_PERIOD;
      elapsed = now - co2CycleStart;
    }

    if (elapsed < CO2_TONE_DURATION_MS) {
      if (getActiveMode() != BUZZER_CO2_ALERT) {
        // An active buzzer must still sound if LEDC cannot provide the preferred timbre.
        if (!startCo2Tone()) digitalWrite(PIN_BUZZER, HIGH);
        setActiveMode(BUZZER_CO2_ALERT);
      }
    } else {
      if (getActiveMode() != BUZZER_SILENT || ledcAttached) forceGpioLow();
      else digitalWrite(PIN_BUZZER, LOW);
      setActiveMode(BUZZER_SILENT);
    }
  }

  void taskLoop() {
    for (;;) {
      service(millis());
      vTaskDelay(pdMS_TO_TICKS(ALARM_OUTPUT_TICK_MS));
    }
  }

public:
  void init() {
    forceGpioLow();
    if (xTaskCreate(taskEntry, "alarmOutput", ALARM_OUTPUT_TASK_STACK_SIZE,
                    this, 2, &workerHandle) != pdPASS) {
      workerHandle = nullptr;
      Serial.println("Alarm output task unavailable; using loop fallback");
    }
  }

  void startTimeAlarm(unsigned long now) {
    state.alarmStartTime = now;
    timeAlarmActive.store(true, std::memory_order_release);
  }

  void dismissTimeAlarm() {
    timeAlarmActive.store(false, std::memory_order_release);
  }

  bool isTimeAlarmActive() const {
    return timeAlarmActive.load(std::memory_order_acquire);
  }

  void setCo2AlarmRequested(bool requested) {
    co2AlarmRequested.store(requested, std::memory_order_release);
  }

  BuzzerMode getActiveMode() const {
    return static_cast<BuzzerMode>(activeMode.load(std::memory_order_acquire));
  }

  void serviceFallback(unsigned long now) {
    if (workerHandle == nullptr) service(now);
  }
} alarmOutputController;

void muteCo2AlertIfRequested() {
  if (isCo2AlertRequested()) state.isCo2Muted = true;
}

void stepWeatherInfoCycle(int direction, unsigned long now) {
  state.weatherInfoCycle = (
    state.weatherInfoCycle + direction + WEATHER_INFO_COUNT
  ) % WEATHER_INFO_COUNT;
  state.lastWeatherInfoCycleChange = now;
  state.weatherTransitionPending = true;
}

void updateWeatherInfoCycle(unsigned long now) {
  if(state.weatherTransitionPending || state.uiState!=SCREEN_SAVER || state.isScreenOff) return;
  if (state.lastWeatherInfoCycleChange == 0) {
    state.lastWeatherInfoCycleChange = now;
    return;
  }

  unsigned long elapsed = now - state.lastWeatherInfoCycleChange;
  const unsigned long period = motionSettings.cycleSeconds * 1000UL;
  if (elapsed < period) return;
  unsigned long steps = elapsed / period;
  state.weatherInfoCycle = (
    state.weatherInfoCycle + static_cast<int>(steps % WEATHER_INFO_COUNT)
  ) % WEATHER_INFO_COUNT;
  state.lastWeatherInfoCycleChange += steps * period;
  state.weatherTransitionPending = true;
}

void updateBuzzerControl(unsigned long now) {
  alarmOutputController.setCo2AlarmRequested(!state.isScreenOff && isCo2AlertRequested());
  alarmOutputController.serviceFallback(now);
}

String urlEncode(const String& input) {
  const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(input.length() * 3);

  for (size_t i = 0; i < input.length(); i++) {
    uint8_t c = static_cast<uint8_t>(input[i]);
    bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (isAlphaNum || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }

  return encoded;
}

bool isWiFiFlowActive() {
  return state.uiState == WIFI_SCANNING || state.uiState == WIFI_LIST ||
         state.uiState == WIFI_PASS_INPUT || state.uiState == WIFI_CONNECTING;
}

bool canRunBackgroundNetworkTask() {
  return !loadingOverlay.active &&
         !isWiFiFlowActive() &&
         state.uiState == SCREEN_SAVER;
}

bool loadingTextRequestsSpinner(const char* text) {
  if (!text || text[0] == '\0') return false;
  if (strstr(text, "...\n")) return true;
  size_t len = strlen(text);
  return len >= 3 && strcmp(text + len - 3, "...") == 0;
}

void showLoadingOverlay(const char* text, bool showSpinner = false, int y = 40) {
  const char* safeText = text ? text : "";
  loadingOverlay.active = true;
  loadingOverlay.showSpinner = showSpinner || loadingTextRequestsSpinner(safeText);
  loadingOverlay.blocksInput = true;
  loadingOverlay.expiresAt = 0;
  loadingOverlay.y = y;
  if (strcmp(loadingOverlay.message, safeText) != 0) {
    copyToBuffer(loadingOverlay.message, sizeof(loadingOverlay.message), safeText);
  }
  state.isScreenOff = false;
  u8g2.setPowerSave(0);
}

void showInfoOverlay(const char* text, unsigned long durationMs = 1600, int y = 40) {
  const char* safeText = text ? text : "";
  loadingOverlay.active = true;
  loadingOverlay.showSpinner = false;
  loadingOverlay.blocksInput = false;
  loadingOverlay.expiresAt = millis() + durationMs;
  loadingOverlay.y = y;
  if (strcmp(loadingOverlay.message, safeText) != 0) {
    copyToBuffer(loadingOverlay.message, sizeof(loadingOverlay.message), safeText);
  }
  state.isScreenOff = false;
  u8g2.setPowerSave(0);
}
void hideLoadingOverlay() {
  loadingOverlay.active = false;
  loadingOverlay.showSpinner = false;
  loadingOverlay.blocksInput = true;
  loadingOverlay.expiresAt = 0;
  loadingOverlay.message[0] = '\0';
  loadingOverlay.y = 40;
}

bool parseJsonResponse(HTTPClient& http, JsonDocument& doc, const JsonDocument& filter) {
  DeserializationError error = deserializeJson(
    doc,
    http.getStream(),
    DeserializationOption::Filter(filter)
  );
  return !error;
}

bool tryParseDoubleInRange(const String& value, double minValue, double maxValue, double& outValue) {
  if (value.length() == 0) return false;
  char* endPtr = nullptr;
  outValue = strtod(value.c_str(), &endPtr);
  if (endPtr == value.c_str()) return false;
  return outValue >= minValue && outValue <= maxValue;
}

bool hasValidLatLon(const String& lat, const String& lon) {
  double parsedLat = 0.0;
  double parsedLon = 0.0;
  return tryParseDoubleInRange(lat, -90.0, 90.0, parsedLat) &&
         tryParseDoubleInRange(lon, -180.0, 180.0, parsedLon);
}

// Smooth the CDS input before using it for contrast or sleep decisions.
void updateAmbientLightState() {
  static unsigned long lastSampleTime = 0;
  unsigned long now = millis();
  if (lastSampleTime != 0 && now - lastSampleTime < 25) return;
  lastSampleTime = now;

  int raw = analogRead(PIN_CDS);
  if (state.filteredCds == 0) {
    state.filteredCds = raw;
  } else {
    state.filteredCds = (state.filteredCds * 7 + raw) / 8;
  }

  int targetContrast = constrain(map(state.filteredCds, 512, 3583, 255, 1), 1, 255);
  int delta = targetContrast - state.displayContrast;
  if (delta > 6) delta = 6;
  else if (delta < -6) delta = -6;
  state.displayContrast = constrain(state.displayContrast + delta, 1, 255);

  const int darkEnterThreshold = DARK_THRESHOLD + 80;
  const int darkExitThreshold = DARK_THRESHOLD - 80;
  if (!state.isAmbientDark) {
    if (state.filteredCds >= darkEnterThreshold) state.isAmbientDark = true;
  } else if (state.filteredCds <= darkExitThreshold) {
    state.isAmbientDark = false;
  }
}

// =================================================================================
// 3. Modules (Classes)
// =================================================================================

class Button {
private:
  int pin;
  oled::DebouncedButton filter;
public:
  Button(int p) : pin(p) {
    pinMode(pin, INPUT_PULLUP);
  }
  bool isTriggered() {
    return filter.update(digitalRead(pin)==LOW,millis(),inputSettings,pin!=PIN_BTN_OK);
  }
  bool down() const { return filter.down(); }
};

Button btnLeft(PIN_BTN_DOWN), btnRight(PIN_BTN_UP), btnOk(PIN_BTN_OK);

class AlarmManager {
private:
  bool hasLastCheckedMinute = false;
  time_t lastCheckedMinuteEpoch = 0;
  long alarmUtcOffset = 0;
  bool pendingOneShotConsumption[MAX_ALARMS] = {};
  enum RunPhase { IDLE, RINGING, LIGHT, KEY_PEEK, KEY_INPUT, CHECK_RING };
  enum PendingPhase : uint8_t { DORMANT, ACTIVE, SNOOZED, CHECK_WAIT, CHECK_ACTIVE };
  // Independent deadlines cost eight bytes per configured-alarm slot. No heap
  // queue, and dismissing a foreground alarm cannot consume a sleeping one.
  struct PendingRun {
    uint32_t started = 0;
    PendingPhase phase = DORMANT;
    uint8_t snoozesUsed = 0;
  } runs[MAX_ALARMS];
  RunPhase runPhase = IDLE;
  AlarmOptions activeOptions;
  unsigned long runStarted = 0, lightHeldSince = 0;
  bool lightHeld = false, keyNeedsRelease = false;
  uint8_t keyPosition = 0, keyPattern = 0;

  bool hasNormalAlarm() const {
    for (int i = 0; i < alarmCount; i++) if (runs[i].phase == ACTIVE) return true;
    return false;
  }

  void refreshActiveOptions() {
    bool first = true;
    PendingPhase selected = hasNormalAlarm() ? ACTIVE : CHECK_ACTIVE;
    activeOptions = AlarmOptions{};
    for (int i = 0; i < alarmCount; i++) {
      if (runs[i].phase != selected) continue;
      AlarmOptions next = options[i];
      // A stored calibration is irrelevant unless this alarm uses light.
      if (!(next.mission & 2) || next.lightRaw > 4095) {
        next.mission &= 1;
        next.lightRaw = 0xffff;
      }
      next.snoozeCount = next.snoozeCount > runs[i].snoozesUsed
                            ? next.snoozeCount - runs[i].snoozesUsed : 0;
      if (first) { activeOptions = next; first = false; }
      else {
        activeOptions.mission |= next.mission;
        activeOptions.lightRaw = min(activeOptions.lightRaw, next.lightRaw);
        activeOptions.snoozeCount = min(activeOptions.snoozeCount, next.snoozeCount);
        activeOptions.snoozeMinutes = min(activeOptions.snoozeMinutes, next.snoozeMinutes);
        activeOptions.checkMinutes = max(activeOptions.checkMinutes, next.checkMinutes);
      }
    }
  }

  void ringAgain(unsigned long now, bool checkOnly = false) {
    refreshActiveOptions();
    runPhase = checkOnly ? CHECK_RING : RINGING;
    runStarted = now;
    lightHeld = false;
    keyNeedsRelease = true;
    alarmOutputController.startTimeAlarm(now);
    state.isScreenOff = false;
    u8g2.setPowerSave(0);
  }

  void finishMission(unsigned long now) {
    dismissCurrentAlarm();
    for (int i = 0; i < alarmCount; i++) if (runs[i].phase == ACTIVE) {
      runs[i].phase = options[i].checkMinutes ? CHECK_WAIT : DORMANT;
      runs[i].started = now;
    }
    runPhase = IDLE;
  }

  void beginKeys(unsigned long now) {
    runPhase = KEY_PEEK; runStarted = now; keyPosition = 0;
    keyPattern = esp_random() & 15; keyNeedsRelease = true;
  }

  bool alarmMatchesMinute(const Alarm& alarm, const struct tm& minuteTime) const {
    bool dayMatch = alarm.days == 0 || ((alarm.days >> minuteTime.tm_wday) & 1);
    return dayMatch && alarm.hour == minuteTime.tm_hour && alarm.minute == minuteTime.tm_min;
  }

public:
  Alarm alarms[MAX_ALARMS];
  AlarmOptions options[MAX_ALARMS];
  int alarmCount = 0;

  bool sessionActive() const {
    for (int i = 0; i < alarmCount; i++) if (runs[i].phase != DORMANT) return true;
    return false;
  }
  bool missionActive() const { return runPhase == LIGHT || runPhase == KEY_PEEK || runPhase == KEY_INPUT; }
  bool keepsScreenAwake() const { return missionActive() || runPhase == CHECK_RING; }
  bool lightMission() const { return runPhase == LIGHT; }
  bool showingKeys() const { return runPhase == KEY_PEEK; }
  bool checkRinging() const { return runPhase == CHECK_RING; }
  uint8_t enteredKeys() const { return keyPosition; }
  bool keyRight(int index) const { return (keyPattern >> index) & 1; }
  uint16_t lightTarget() const { return activeOptions.lightRaw; }
  unsigned long lightProgress(unsigned long now) const { return lightHeld ? now-lightHeldSince : 0; }

  void acknowledge(unsigned long now, bool snoozeRequested) {
    if (runPhase != RINGING && runPhase != CHECK_RING) return;
    // One acknowledgement answers visible wake checks, without touching
    // checks or snoozes whose independent deadlines have not arrived yet.
    for (int i = 0; i < alarmCount; i++)
      if (runs[i].phase == CHECK_ACTIVE) runs[i].phase = DORMANT;
    if(runPhase == CHECK_RING) { alarmOutputController.dismissTimeAlarm();runPhase=IDLE;return; }
    if(snoozeRequested && activeOptions.snoozeCount) {
      for (int i = 0; i < alarmCount; i++) if (runs[i].phase == ACTIVE) {
        ++runs[i].snoozesUsed; runs[i].phase=SNOOZED; runs[i].started=now;
      }
      alarmOutputController.dismissTimeAlarm();runPhase=IDLE;return;
    }
    if(activeOptions.mission == 0) { finishMission(now);return; }
    alarmOutputController.dismissTimeAlarm();runStarted=now;lightHeld=false;
    if(activeOptions.mission & 2) runPhase=LIGHT; else beginKeys(now);
  }

  void missionInput(bool leftEvent, bool rightEvent, bool buttonsHeld, unsigned long now) {
    if(keyNeedsRelease) { if(!buttonsHeld) keyNeedsRelease=false;return; }
    if(runPhase != KEY_INPUT || leftEvent == rightEvent) return;
    keyNeedsRelease=true;
    if(rightEvent == keyRight(keyPosition)) { if(++keyPosition==4) finishMission(now); }
    else beginKeys(now);
  }

  void service(unsigned long now) {
    bool ready = false;
    for (int i = 0; i < alarmCount; i++) {
      if (runs[i].phase == SNOOZED && uint32_t(now-runs[i].started) >= options[i].snoozeMinutes*60000UL) {
        runs[i].phase = ACTIVE; ready = true;
      } else if (runs[i].phase == CHECK_WAIT && uint32_t(now-runs[i].started) >= options[i].checkMinutes*60000UL) {
        runs[i].phase = CHECK_ACTIVE; runs[i].started = now; ready = true;
      } else if (runs[i].phase == CHECK_ACTIVE && uint32_t(now-runs[i].started) >= options[i].checkTimeoutSec*1000UL) {
        // Each visible check owns its response window; a later check must not
        // restart it, and a shorter window must not expire another alarm early.
        runs[i].phase = ACTIVE; ready = true;
      }
    }
    if (ready) ringAgain(now, !hasNormalAlarm());
    if(runPhase==LIGHT) {
      // This CDS circuit reports a LOWER ADC value in brighter surroundings.
      if(state.filteredCds <= static_cast<int>(activeOptions.lightRaw)+80) {
        if(!lightHeld){lightHeld=true;lightHeldSince=now;}
        if(now-lightHeldSince>=3000) { if(activeOptions.mission&1)beginKeys(now);else finishMission(now); }
      } else lightHeld=false;
      if(runPhase==LIGHT && now-runStarted>=120000UL)ringAgain(now);
    } else if(runPhase==KEY_PEEK && now-runStarted>=1900) {runPhase=KEY_INPUT;runStarted=now;}
    else if(runPhase==KEY_INPUT && now-runStarted>=120000UL)ringAgain(now);
  }

  void recheckCurrentMinute() {
    if (!state.isTimeSynced) {
      hasLastCheckedMinute = false;
      return;
    }
    time(&lastCheckedMinuteEpoch);
    lastCheckedMinuteEpoch -= lastCheckedMinuteEpoch % 60;
    lastCheckedMinuteEpoch -= 60;
    hasLastCheckedMinute = true;
  }

  void setCalendarOffset(long offsetSeconds) {
    if (alarmUtcOffset == offsetSeconds) return;
    alarmUtcOffset = offsetSeconds;
    recheckCurrentMinute();
  }

  void dismissCurrentAlarm() {
    alarmOutputController.dismissTimeAlarm();
    bool oneShotAlarmChanged = false;
    for (int i = 0; i < alarmCount; i++) {
      if (runs[i].phase != ACTIVE) continue;
      if (pendingOneShotConsumption[i] && alarms[i].days == 0 && alarms[i].enabled) {
        alarms[i].enabled = false;
        oneShotAlarmChanged = true;
      }
      pendingOneShotConsumption[i] = false;
    }
    if (oneShotAlarmChanged) save();
  }

  void sanitizeAlarm(Alarm& alarm) {
    alarm.hour %= 24;
    alarm.minute %= 60;
    alarm.days &= 0x7F;
    alarm.enabled = alarm.enabled ? true : false;
  }

  void load() {
    prefs.begin("clock", true);
    uint32_t storedCount = prefs.getUInt("cnt", 0);
    size_t storedBytes = prefs.getBytesLength("data");
    alarmCount = 0;
    if (storedCount <= MAX_ALARMS && storedBytes >= sizeof(Alarm) * storedCount) {
      alarmCount = storedCount;
      if (alarmCount > 0) prefs.getBytes("data", alarms, sizeof(Alarm) * alarmCount);
    }
    for (int i = 0; i < alarmCount; i++) sanitizeAlarm(alarms[i]);

    if(prefs.getBytesLength("alarmOpt")==sizeof(AlarmOptions)*alarmCount) {
      if(alarmCount) prefs.getBytes("alarmOpt",options,sizeof(AlarmOptions)*alarmCount);
      for(int i=0;i<alarmCount;i++) {
        options[i].snoozeMinutes=clampValue(static_cast<int>(options[i].snoozeMinutes),1,20);
        options[i].snoozeCount=clampValue(static_cast<int>(options[i].snoozeCount),0,5);
        options[i].checkMinutes=clampValue(static_cast<int>(options[i].checkMinutes),0,10);
        options[i].checkTimeoutSec=clampValue(static_cast<int>(options[i].checkTimeoutSec),10,180);
        options[i].mission&=3;
        if(options[i].lightRaw>4095){options[i].lightRaw=0xffff;options[i].mission&=1;}
      }
    }
    motionSettings.speedHalf=clampValue(prefs.getInt("moveSpeed",12),2,36);
    motionSettings.curve=clampValue(prefs.getInt("moveCurve",55),0,100);
    motionSettings.coverage=clampValue(prefs.getInt("moveCover",70),0,100);
    motionSettings.cycleSeconds=clampValue(prefs.getInt("moveCycle",3),2,10);
    motionSettings.organic=prefs.getBool("moveOrganic",true);
    motionSettings.jerk=clampValue(prefs.getInt("moveJerk",60),0,100);
    inputSettings.debounceMs=clampValue(int(prefs.getUInt("btnDebounce",5)),5,50);
    inputSettings.holdDelayMs=clampValue(int(prefs.getUInt("btnHold",oled::InputSettings{}.holdDelayMs)),int(oled::InputSettings::MIN_HOLD_MS),int(oled::InputSettings::MAX_HOLD_MS));
    inputSettings.repeatDelayMs=clampValue(int(prefs.getUInt("btnRepeatWait",oled::InputSettings{}.repeatDelayMs)),int(oled::InputSettings::MIN_REPEAT_MS),int(oled::InputSettings::MAX_REPEAT_MS));
    inputSettings.repeatHz=clampValue(int(prefs.getUInt("btnRepeat",10)),2,10);
    inputSettings.chordMs=clampValue(int(prefs.getUInt("btnChord",60)),30,120);

    state.co2Limit = clampValue(prefs.getInt("co2", 1500), 500, 3100);
    state.abcEnabled = prefs.getBool("abc", true);
    copyToBuffer(state.cityName, sizeof(state.cityName), prefs.getString("locationName", ""));
    state.lat = prefs.getString("locationLat", "");
    state.lon = prefs.getString("locationLon", "");
    bool hasSavedLocation = hasValidLatLon(state.lat, state.lon);
    long savedUtcOffset = prefs.getLong("locationUtc", 0);
    rtc.offset = hasSavedLocation && savedUtcOffset >= -43200 && savedUtcOffset <= 50400
                   ? savedUtcOffset
                   : 0;
    alarmUtcOffset = rtc.offset;
    memset(pendingOneShotConsumption, 0, sizeof(pendingOneShotConsumption));
    for (auto& run : runs) run = PendingRun{};
    runPhase = IDLE;
    if (!hasSavedLocation) {
      state.lat = "";
      state.lon = "";
      state.cityName[0] = '\0';
    }
    state.wifiAutoReconnectEnabled = prefs.getBool("wifiAuto", true);
    prefs.end();
  }

  void save() {
    alarmCount = clampValue(alarmCount, 0, MAX_ALARMS);
    for (int i = 0; i < alarmCount; i++) sanitizeAlarm(alarms[i]);

    prefs.begin("clock", false);
    prefs.putUInt("cnt", alarmCount);
    prefs.putBytes("data", alarms, sizeof(Alarm) * alarmCount);
    if(alarmCount) prefs.putBytes("alarmOpt",options,sizeof(AlarmOptions)*alarmCount); else prefs.remove("alarmOpt");
    prefs.putInt("co2", state.co2Limit);
    prefs.putBool("abc", state.abcEnabled);
    prefs.putString("locationName", state.cityName);
    prefs.putString("locationLat", state.lat);
    prefs.putString("locationLon", state.lon);
    prefs.putLong("locationUtc", rtc.offset);
    prefs.putBool("wifiAuto", state.wifiAutoReconnectEnabled);
    prefs.end();
  }

  void check() {
    if (!state.isTimeSynced) return;

    time_t currentMinuteEpoch;
    time(&currentMinuteEpoch);
    currentMinuteEpoch -= currentMinuteEpoch % 60;
    if (hasLastCheckedMinute && currentMinuteEpoch == lastCheckedMinuteEpoch) return;

    const time_t oneMinute = 60;
    const time_t oldestAllowedMinute = currentMinuteEpoch - ALARM_LOOKBACK_MINUTES * oneMinute;
    time_t firstMinuteToCheck = oldestAllowedMinute;

    if (hasLastCheckedMinute) {
      if (currentMinuteEpoch > lastCheckedMinuteEpoch) {
        firstMinuteToCheck = lastCheckedMinuteEpoch + oneMinute;
        if (firstMinuteToCheck < oldestAllowedMinute) firstMinuteToCheck = oldestAllowedMinute;
      } else {
        // If NTP moves the clock backward, establish a new calendar baseline
        // without replaying the whole already-checked interval.
        firstMinuteToCheck = currentMinuteEpoch;
      }
    }

    // Advance the checkpoint before activating the alarm. Dismissing an alarm
    // in the same minute therefore cannot replay that minute.
    lastCheckedMinuteEpoch = currentMinuteEpoch;
    hasLastCheckedMinute = true;

    bool shouldRing = false;
    for (time_t minuteEpoch = firstMinuteToCheck;
         minuteEpoch <= currentMinuteEpoch;
         minuteEpoch += oneMinute) {
      time_t localMinuteEpoch = minuteEpoch + alarmUtcOffset;
      struct tm minuteTime;
      gmtime_r(&localMinuteEpoch, &minuteTime);

      for (int i = 0; i < alarmCount; i++) {
        if (!alarms[i].enabled || runs[i].phase != DORMANT ||
            !alarmMatchesMinute(alarms[i], minuteTime)) continue;
        runs[i].phase = ACTIVE;
        runs[i].snoozesUsed = 0;
        shouldRing = true;
        if (alarms[i].days == 0) pendingOneShotConsumption[i] = true;
      }
    }

    if (shouldRing) {
      ringAgain(millis());
    }
  }
} alarmMgr;

// This preference is both the reconnect policy and the guard that keeps the
// radio from coming back on by itself after a manual disconnect.
void applyWiFiAutoReconnectPreference(bool enabled) {
  if (state.wifiAutoReconnectEnabled == enabled) {
    WiFi.setAutoReconnect(enabled);
    return;
  }

  state.wifiAutoReconnectEnabled = enabled;
  WiFi.setAutoReconnect(enabled);
  alarmMgr.save();
}

void ensureWiFiStationMode() {
  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(state.wifiAutoReconnectEnabled);
}

void powerDownWiFiIfIdle();

// All MH-Z19 UART traffic is serialized through one worker task so the main
// loop never blocks on sensor reads.
class SensorManager {
private:
  SemaphoreHandle_t serialMutex = nullptr;
  SemaphoreHandle_t readingMutex = nullptr;
  TaskHandle_t workerHandle = nullptr;
  SensorReading latestReading;
  bool hasUnreadReading = false;

  static void taskEntry(void* param) {
    static_cast<SensorManager*>(param)->taskLoop();
  }

  bool tryLockSerial(TickType_t timeoutTicks = portMAX_DELAY) {
    return serialMutex && xSemaphoreTake(serialMutex, timeoutTicks) == pdTRUE;
  }

  void unlockSerial() {
    if (serialMutex) xSemaphoreGive(serialMutex);
  }

  void storeLatestReading(int value) {
    if (!readingMutex) return;
    if (xSemaphoreTake(readingMutex, portMAX_DELAY) == pdTRUE) {
      latestReading.co2 = value;
      latestReading.valid = value > 0;
      hasUnreadReading = true;
      xSemaphoreGive(readingMutex);
    }
  }

  int readMHZ19Locked() {
    byte cmd[9] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
    while (sensorSerial.available()) sensorSerial.read();
    sensorSerial.write(cmd, 9);
    unsigned long start = millis();
    while (sensorSerial.available() < 9) {
      if (millis() - start > 500) return -2;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    byte response[9];
    sensorSerial.readBytes(response, 9);
    if (response[0] != 0xFF || response[1] != 0x86) return -3;
    byte checksum = 0;
    for (int i = 1; i < 8; i++) checksum += response[i];
    if (response[8] != (byte)(0xFF - checksum + 1)) return -1;
    return (256 * (int)response[2]) + (int)response[3];
  }

  void taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
      bool hasSerialAccess = false;
      int reading = -2;
      if (tryLockSerial(pdMS_TO_TICKS(600))) {
        hasSerialAccess = true;
        reading = readMHZ19Locked();
        unlockSerial();
      }
      if (hasSerialAccess) storeLatestReading(reading);
      vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SENSOR_READ_PERIOD));
    }
  }

public:
  void init() {
    if (!serialMutex) serialMutex = xSemaphoreCreateMutex();
    if (!readingMutex) readingMutex = xSemaphoreCreateMutex();
    sensorSerial.setRxBufferSize(64);
    sensorSerial.setTimeout(20);
    sensorSerial.begin(9600, SERIAL_8N1, PIN_RX, PIN_TX);
    setAutoCalibration(state.abcEnabled);
    if (serialMutex && readingMutex && !workerHandle) {
      xTaskCreate(taskEntry, "sensorTask", SENSOR_TASK_STACK_SIZE, this, 1, &workerHandle);
    }
  }

  bool applyLatestReading() {
    if (!readingMutex) return false;

    SensorReading reading;
    bool hasReading = false;
    if (xSemaphoreTake(readingMutex, 0) == pdTRUE) {
      if (hasUnreadReading) {
        reading = latestReading;
        hasUnreadReading = false;
        hasReading = true;
      }
      xSemaphoreGive(readingMutex);
    }
    if (!hasReading) return false;

    if (reading.valid) {
      state.co2 = reading.co2;
      state.isSensorError = false;
    } else {
      state.isSensorError = true;
    }

    if (state.co2 + 50 < state.co2Limit) state.isCo2Muted = false;
    return true;
  }

  void setAutoCalibration(bool enable) {
    byte cmd[9];
    byte on[] = { 0xFF, 0x01, 0x79, 0xA0, 0x00, 0x00, 0x00, 0x00, 0xE6 };
    byte off[] = { 0xFF, 0x01, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86 };
    memcpy(cmd, enable ? on : off, 9);
    // Explicit setup commands wait for the current (at most 500 ms) read so
    // bytes can never interleave on the sensor UART.
    if (tryLockSerial(portMAX_DELAY)) {
      sensorSerial.write(cmd, 9);
      sensorSerial.flush();
      unlockSerial();
    }
  }

  void calibrateBaseline() {
    byte cmd[] = { 0xFF, 0x01, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78 };
    // Calibration is rare and must not be dropped or overlap a sensor read.
    if (tryLockSerial(portMAX_DELAY)) {
      sensorSerial.write(cmd, 9);
      sensorSerial.flush();
      unlockSerial();
    }
  }

  bool fetchWeatherForLocation(const char* targetLat, const char* targetLon, WeatherSnapshot& outWeather) {
    if (WiFi.status() != WL_CONNECTED || !targetLat || !targetLon ||
        targetLat[0] == '\0' || targetLon[0] == '\0') return false;

    bool weatherSuccess = false;

    {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(HTTP_TIMEOUT);
      http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      String urlW = "http://api.open-meteo.com/v1/forecast?latitude=";
      urlW += targetLat;
      urlW += "&longitude=";
      urlW += targetLon;
      urlW += "&current_weather=true&daily=precipitation_probability_max&timezone=auto";
      http.begin(client, urlW);
      if (http.GET() == 200) {
        JsonDocument doc;
        JsonDocument filter;
        filter["current_weather"]["temperature"] = true;
        filter["current_weather"]["weathercode"] = true;
        filter["current_weather"]["is_day"] = true;
        filter["current_weather"]["windspeed"] = true;
        filter["current_weather"]["winddirection"] = true;
        filter["daily"]["precipitation_probability_max"][0] = true;
        filter["utc_offset_seconds"] = true;
        if (parseJsonResponse(http, doc, filter) && doc["utc_offset_seconds"].is<long>()) {
          long utcOffsetSeconds = doc["utc_offset_seconds"].as<long>();
          if (utcOffsetSeconds < -43200 || utcOffsetSeconds > 50400) {
            http.end();
            client.stop();
            return false;
          }
          outWeather.utcOffsetSeconds = utcOffsetSeconds;
          outWeather.tempOut = doc["current_weather"]["temperature"];
          outWeather.weatherCode = doc["current_weather"]["weathercode"];
          outWeather.isDay = doc["current_weather"]["is_day"];
          outWeather.windSpeed = doc["current_weather"]["windspeed"];
          outWeather.windDir = doc["current_weather"]["winddirection"];
          outWeather.rainProb = doc["daily"]["precipitation_probability_max"][0];
          weatherSuccess = true;
        }
      }
      http.end();
      client.stop();
    }

    {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(HTTP_TIMEOUT);
      http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      String urlA = "http://air-quality-api.open-meteo.com/v1/air-quality?latitude=";
      urlA += targetLat;
      urlA += "&longitude=";
      urlA += targetLon;
      urlA += "&current=pm10";
      http.begin(client, urlA);
      if (http.GET() == 200) {
        JsonDocument doc;
        JsonDocument filter;
        filter["current"]["pm10"] = true;
        if (parseJsonResponse(http, doc, filter)) {
          outWeather.pm10 = doc["current"]["pm10"];
        }
      }
      http.end();
      client.stop();
    }

    return weatherSuccess;
  }

  bool searchLocationList(String query, PackedLocationResult* outResults,
                          int maxResults, int& outCount) {
    outCount = 0;
    if (!outResults || maxResults <= 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    query.trim();
    if (query.length() == 0) return false;

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT);
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    String url = "http://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(query) +
                 "&count=20&language=en&format=json";
    http.begin(client, url);
    if (http.GET() == 200) {
      JsonDocument doc;
      JsonDocument filter;
      filter["results"][0]["name"] = true;
      filter["results"][0]["country"] = true;
      filter["results"][0]["latitude"] = true;
      filter["results"][0]["longitude"] = true;
      if (parseJsonResponse(http, doc, filter) &&
          doc["results"].is<JsonArray>() && doc["results"].size() > 0) {
        JsonArray results = doc["results"].as<JsonArray>();
        for (JsonObject repo : results) {
          if (outCount >= maxResults) break;
          PackedLocationResult& item = outResults[outCount++];
          copyToBuffer(item.name, sizeof(item.name), repo["name"] | "");
          copyToBuffer(item.country, sizeof(item.country), repo["country"] | "");
          serializeJson(repo["latitude"], item.lat, sizeof(item.lat));
          serializeJson(repo["longitude"], item.lon, sizeof(item.lon));
        }
      }
    }
    http.end();
    client.stop();
    return outCount > 0;
  }
} sensorMgr;


// The app runs one network worker task so blocking HTTP never stalls input or
// rendering. Main-loop callers only enqueue jobs and drain finished results.
class AppNetworkWorker {
private:
  SemaphoreHandle_t mutex = nullptr;
  TaskHandle_t workerHandle = nullptr;
  NetworkJob queuedJob;
  bool hasQueuedJob = false;
  bool busy = false;
  NetworkResult completedResult;
  bool hasCompletedResult = false;

  static void taskEntry(void* param) {
    static_cast<AppNetworkWorker*>(param)->taskLoop();
  }

  void taskLoop() {
    for (;;) {
      NetworkJob job;
      bool hasJob = false;

      if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
        if (hasQueuedJob && !busy) {
          job = queuedJob;
          hasQueuedJob = false;
          busy = true;
          hasJob = true;
        }
        xSemaphoreGive(mutex);
      }

      if (!hasJob) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }

      NetworkResult workingResult;
      workingResult.type = job.type;
      workingResult.context = job.context;

      if (job.type == NETWORK_JOB_WEATHER_SYNC) {
        workingResult.success = executeWeatherSync(job, workingResult);
        workingResult.hasWeatherData = workingResult.success;
      } else if (job.type == NETWORK_JOB_LOCATION_SEARCH) {
        workingResult.success = executeLocationSearch(job, workingResult);
        workingResult.hasLocationResults = true;
      }

      if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
        completedResult = workingResult;
        hasCompletedResult = true;
        busy = false;
        xSemaphoreGive(mutex);
      }
    }
  }

  bool executeWeatherSync(const NetworkJob& job, NetworkResult& result) {
    WeatherSnapshot weather;
    if (!sensorMgr.fetchWeatherForLocation(job.lat, job.lon, weather)) return false;

    copyToBuffer(result.lat, sizeof(result.lat), job.lat);
    copyToBuffer(result.lon, sizeof(result.lon), job.lon);
    result.utcOffsetSeconds = weather.utcOffsetSeconds;
    result.tempOut = weather.tempOut;
    result.weatherCode = weather.weatherCode;
    result.isDay = weather.isDay;
    result.rainProb = weather.rainProb;
    result.pm10 = weather.pm10;
    result.windSpeed = weather.windSpeed;
    result.windDir = weather.windDir;
    return true;
  }

  bool executeLocationSearch(const NetworkJob& job, NetworkResult& result) {
    return sensorMgr.searchLocationList(
      job.query,
      result.locationResults,
      MAX_LOCATION_RESULTS,
      result.locationCount
    );
  }

  bool readBusyState(bool includeCompletedResult) {
    if (!mutex) return false;

    // Failure to inspect the worker is treated as busy, never as permission
    // to start a conflicting radio operation.
    bool currentBusy = true;
    if (xSemaphoreTake(mutex, 0) == pdTRUE) {
      currentBusy = busy || hasQueuedJob || (includeCompletedResult && hasCompletedResult);
      xSemaphoreGive(mutex);
    }
    return currentBusy;
  }

  bool enqueue(const NetworkJob& job) {
    if (!mutex) return false;

    bool accepted = false;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(NETWORK_ENQUEUE_TIMEOUT_MS)) == pdTRUE) {
      if (!busy && !hasQueuedJob && !hasCompletedResult) {
        queuedJob = job;
        hasQueuedJob = true;
        accepted = true;
      }
      xSemaphoreGive(mutex);
    }
    return accepted;
  }

public:
  void init() {
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (mutex && !workerHandle) {
      xTaskCreate(taskEntry, "networkTask", NETWORK_TASK_STACK_SIZE, this, 1, &workerHandle);
    }
  }

  bool requestWeatherSync(NetworkJobContext context, const String& lat, const String& lon) {
    NetworkJob job;
    job.type = NETWORK_JOB_WEATHER_SYNC;
    job.context = context;
    copyToBuffer(job.lat, sizeof(job.lat), lat);
    copyToBuffer(job.lon, sizeof(job.lon), lon);
    return enqueue(job);
  }

  bool requestLocationSearch(const String& query) {
    NetworkJob job;
    job.type = NETWORK_JOB_LOCATION_SEARCH;
    copyToBuffer(job.query, sizeof(job.query), query);
    return enqueue(job);
  }

  bool isBusy() {
    return readBusyState(true);
  }

  bool hasActiveWork() {
    return readBusyState(false);
  }

  bool takeResult(NetworkResult& result) {
    if (!mutex) return false;

    bool hasResult = false;
    if (xSemaphoreTake(mutex, 0) == pdTRUE) {
      if (hasCompletedResult) {
        result = completedResult;
        hasCompletedResult = false;
        hasResult = true;
      }
      xSemaphoreGive(mutex);
    }
    return hasResult;
  }
} appNetworkWorker;

// Cheap guards run before the non-blocking worker check, so the normal path
// does not touch the network mutex every frame.
void powerDownWiFiIfIdle() {
  if (state.wifiAutoReconnectEnabled) return;
  if (startupFlow.phase != STARTUP_IDLE) return;
  if (loadingOverlay.active || isWiFiFlowActive()) return;
  if (WiFi.status() == WL_CONNECTED) return;
  if (WiFi.getMode() == WIFI_OFF) return;
  if (appNetworkWorker.isBusy()) return;
  WiFi.disconnect(true, false);
}

class WiFiStorageManager {
public:
  SavedWiFi savedNets[MAX_SAVED_WIFI];
  int count = 0;
  uint32_t currentTick = 0;

  void load() {
    prefs.begin("wifi_db", true);
    count = prefs.getUInt("cnt", 0);
    currentTick = prefs.getUInt("tick", 0);
    if (count > 0 && count <= MAX_SAVED_WIFI &&
        prefs.getBytesLength("data") >= sizeof(SavedWiFi) * count) {
      prefs.getBytes("data", savedNets, sizeof(SavedWiFi) * count);
      for (int i = 0; i < count; i++) {
        savedNets[i].ssid[32] = '\0';
        savedNets[i].pass[64] = '\0';
      }
    } else {
      count = 0;
    }
    prefs.end();
  }

  void save() {
    prefs.begin("wifi_db", false);
    prefs.putUInt("cnt", count);
    prefs.putUInt("tick", currentTick);
    prefs.putBytes("data", savedNets, sizeof(SavedWiFi) * count);
    prefs.end();
  }

  void remove(const char* ssid) {
    for (int i = 0; i < count; i++) {
      if (strcmp(savedNets[i].ssid, ssid) == 0) {
        for (int j = i; j < count - 1; j++) savedNets[j] = savedNets[j + 1];
        count--;
        save();
        return;
      }
    }
  }

  void addOrUpdate(const char* ssid, const char* pass) {
    currentTick++;
    for (int i = 0; i < count; i++) {
      if (strcmp(savedNets[i].ssid, ssid) == 0) {
        copyToBuffer(savedNets[i].pass, sizeof(savedNets[i].pass), pass);
        savedNets[i].lastUsedTick = currentTick;
        save();
        return;
      }
    }

    if (count >= MAX_SAVED_WIFI) {
      int oldestIdx = 0;
      for (int i = 1; i < count; i++) {
        if (savedNets[i].lastUsedTick < savedNets[oldestIdx].lastUsedTick) oldestIdx = i;
      }
      copyToBuffer(savedNets[oldestIdx].ssid, sizeof(savedNets[oldestIdx].ssid), ssid);
      copyToBuffer(savedNets[oldestIdx].pass, sizeof(savedNets[oldestIdx].pass), pass);
      savedNets[oldestIdx].lastUsedTick = currentTick;
    } else {
      copyToBuffer(savedNets[count].ssid, sizeof(savedNets[count].ssid), ssid);
      copyToBuffer(savedNets[count].pass, sizeof(savedNets[count].pass), pass);
      savedNets[count].lastUsedTick = currentTick;
      count++;
    }
    save();
  }

  bool getPassword(const char* ssid, char* passBuf) {
    for (int i = 0; i < count; i++) {
      if (strcmp(savedNets[i].ssid, ssid) == 0) {
        strncpy(passBuf, savedNets[i].pass, 64);
        passBuf[64] = '\0';
        return true;
      }
    }
    return false;
  }
} wifiStorage;

// Wi-Fi setup keeps scan results and system actions in one list so the menu can
// treat them with the same cursor logic.
class APListManager {
public:
  std::vector<APItem> apList;
  bool isScanning = false;
  unsigned long scanStartTime = 0;
  char connectingSSID[33] = "";
  char connectingPass[65] = "";
  unsigned long connectStartTime = 0;
  bool showFailMessage = false;
  WiFiScanMessage scanMessage = WIFI_SCAN_MESSAGE_NONE;
  int connectStatus = 0;

  void releaseList() {
    std::vector<APItem>().swap(apList);
  }

  void moveAPToTop(const String& ssid) {
    for (size_t i = 0; i < apList.size(); i++) {
      if (apList[i].ssid == ssid) {
        APItem temp = apList[i];
        apList.erase(apList.begin() + i);
        size_t insertIdx = 0;
        while (insertIdx < apList.size() && apList[insertIdx].category < 0) insertIdx++;
        apList.insert(apList.begin() + insertIdx, temp);
        return;
      }
    }
  }

  const char* getPowerItemLabel() const {
    return (WiFi.getMode() == WIFI_OFF) ? "[ WiFi: Off ]" : "[ WiFi: On ]";
  }

  void refreshSystemItems() {
    for (auto& item : apList) {
      if (item.category == -3) item.ssid = "< Back";
      else if (item.category == -2) item.ssid = getPowerItemLabel();
      else if (item.category == -1) item.ssid = "[ Rescan AP ]";
    }
  }

  void startScan(WiFiScanMessage nextMessage = WIFI_SCAN_MESSAGE_NONE) {
    isScanning = true;
    scanStartTime = millis();
    connectStatus = 0;
    connectingSSID[0] = '\0';
    connectingPass[0] = '\0';
    scanMessage = nextMessage;
    showFailMessage = (nextMessage == WIFI_SCAN_MESSAGE_CONNECTION_FAILED);
    apList.clear();
    ensureWiFiStationMode();
    WiFi.scanDelete();
    WiFi.scanNetworks(true, false);
  }

  int findIndexBySSID(const String& ssid) const {
    for (size_t i = 0; i < apList.size(); i++) {
      if (apList[i].ssid == ssid) return static_cast<int>(i);
    }
    return 0;
  }
  int findIndexBySSID(const char* ssid) const {
    if (!ssid || ssid[0] == '\0') return 0;
    for (size_t i = 0; i < apList.size(); i++) {
      if (apList[i].ssid == ssid) return static_cast<int>(i);
    }
    return 0;
  }

  int findIndexByCategory(int category) const {
    for (size_t i = 0; i < apList.size(); i++) {
      if (apList[i].category == category) return static_cast<int>(i);
    }
    return 0;
  }

  uint16_t getSignalIcon(int rssi) {
    if (rssi == -100) return 57879;
    if (rssi < -80) return 57880;
    if (rssi < -60) return 57881;
    return 57882;
  }

  void sortList() {
    std::sort(apList.begin(), apList.end(), [](const APItem& a, const APItem& b) {
      if (a.category != b.category) return a.category < b.category;
      if (a.rssi != b.rssi) return a.rssi > b.rssi;
      return a.ssid.compareTo(b.ssid) < 0;
    });
  }

  void buildAndSortList(int scannedCount) {
    apList.clear();
    apList.reserve(scannedCount + wifiStorage.count + 3);
    bool wifiIsOff = (WiFi.getMode() == WIFI_OFF);
    String currentSSID = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "";
    String currentBSSID = (WiFi.status() == WL_CONNECTED) ? WiFi.BSSIDstr() : "";
    if (!wifiIsOff) {
      for (int i = 0; i < scannedCount; ++i) {
        String ssid = WiFi.SSID(i);
        String bssid = WiFi.BSSIDstr(i);
        if (ssid.length() == 0) continue;

        APItem item;
        item.ssid = ssid;
        item.bssid = bssid;
        item.rssi = WiFi.RSSI(i);
        item.isSecured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        item.hasDuplicate = false;

        if (WiFi.status() == WL_CONNECTED && ssid == currentSSID && bssid.equalsIgnoreCase(currentBSSID)) {
          item.category = 0;
        } else {
          char dummy[65];
          if (wifiStorage.getPassword(ssid.c_str(), dummy)) item.category = 1;
          else item.category = 2;
        }
        apList.push_back(item);
      }

      for (int i = 0; i < wifiStorage.count; i++) {
        bool found = false;
        for (const auto& ap : apList) {
          if (ap.ssid == wifiStorage.savedNets[i].ssid) {
            found = true;
            break;
          }
        }
        if (!found) {
          APItem item;
          item.ssid = wifiStorage.savedNets[i].ssid;
          item.bssid = "";
          item.rssi = -100;
          item.isSecured = true;
          item.category = 3;
          item.hasDuplicate = false;
          apList.push_back(item);
        }
      }
    }

    for (size_t i = 0; i < apList.size(); i++) {
      apList[i].hasDuplicate = false;
      for (size_t j = 0; j < apList.size(); j++) {
        if (i != j && apList[i].ssid == apList[j].ssid && apList[i].category >= 0) {
          apList[i].hasDuplicate = true;
          break;
        }
      }
    }

    apList.push_back({ "< Back", "", -100, false, -3, false });
    apList.push_back({ getPowerItemLabel(), "", -100, false, -2, false });
    if (!wifiIsOff) apList.push_back({ "[ Rescan AP ]", "", -100, false, -1, false });
    sortList();
  }
} apMgr;

struct BouncingElement {
  float x, y, dirX, dirY;
  int w, h;

  void enforceMinimumSpeed() {
    const float minComponent = 0.018f;
    const float minTotalSpeed = 0.055f;

    if (fabsf(dirX) < minComponent) dirX = (dirX < 0.0f) ? -minComponent : minComponent;
    if (fabsf(dirY) < minComponent) dirY = (dirY < 0.0f) ? -minComponent : minComponent;

    float speed = sqrtf(dirX * dirX + dirY * dirY);
    if (speed < minTotalSpeed) {
      float scale = minTotalSpeed / speed;
      dirX *= scale;
      dirY *= scale;
    }
  }

  void clampInside() {
    if (x < 0.0f) {
      x = 0;
    } else if (x + w > SCREEN_W) {
      x = SCREEN_W - w;
    }

    if (y < 0.0f) {
      y = 0;
    } else if (y + h > SCREEN_H) {
      y = SCREEN_H - h;
    }
  }

  void update() {
    enforceMinimumSpeed();
    x += dirX;
    y += dirY;
    clampInside();
  }

  int drawX() const {
    if (x < 0.5f) return 0;
    if (x + w > SCREEN_W - 0.5f) return SCREEN_W - w;
    int px = static_cast<int>(roundf(x));
    if (px < 0) return 0;
    if (px > SCREEN_W - w) return SCREEN_W - w;
    return px;
  }

  int drawY() const {
    if (y < 0.5f) return 0;
    if (y + h > SCREEN_H - 0.5f) return SCREEN_H - h;
    int py = static_cast<int>(roundf(y));
    if (py < 0) return 0;
    if (py > SCREEN_H - h) return SCREEN_H - h;
    return py;
  }
};

// Text input is built around the two horizontal buttons plus OK, so it keeps a
// compact local state machine for scroll, repeat and cursor modes.
class TextInputManager {
private:
  // The longest remaining input is a 64-character WiFi password.
  static const int INPUT_BUFFER_CAPACITY = 65;
  const char* charSets[4] = { "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789", "!@#$%^&*()_+-=[]{}|;':\",./<>?" };
  const char* specialKeys[5] = { "DEL", "OK", "CLR", "ESC", "CUR" };
  int charSetLengths[5] = { 26, 26, 10, 0, 5 };

  int currentMode = 0;
  int focusIdx[5] = { 0, 0, 0, 0, 0 };
  String inputString = "";
  int maxLen = 30;

  unsigned long firstPressTime = 0;
  int pendingBtn = 0;
  bool dualTriggered = false;
  bool actionExecuted = false;
  unsigned long lastRepeatTime = 0;
  int repeatDelay = 500;

  int cursorPos = 0;
  bool isCursorMode = false;
  int displayOffset = 0;
  oled::CarouselSlide carousel;
  bool inputPulse=false;

  void executeScroll(int btn) {
    if (isCursorMode) {
      if (btn == 1 && cursorPos > 0) cursorPos--;
      if (btn == 2 && cursorPos < (int)inputString.length()) cursorPos++;
    } else {
      int len = charSetLengths[currentMode];
      if (btn == 1) focusIdx[currentMode] = (focusIdx[currentMode] - 1 + len) % len;
      if (btn == 2) focusIdx[currentMode] = (focusIdx[currentMode] + 1) % len;
      carousel.move(btn==1?-1:1,millis());
    }
  }

  void insertCharAtCursor(char ch) {
    if (inputString.length() >= maxLen) return;

    size_t len = inputString.length();
    if (cursorPos < 0) cursorPos = 0;
    if (static_cast<size_t>(cursorPos) > len) cursorPos = static_cast<int>(len);
    if (len + 1 >= INPUT_BUFFER_CAPACITY) return;

    char buffer[INPUT_BUFFER_CAPACITY];
    inputString.toCharArray(buffer, sizeof(buffer));
    memmove(buffer + cursorPos + 1, buffer + cursorPos, len - cursorPos + 1);
    buffer[cursorPos] = ch;
    inputString = buffer;
    cursorPos++;
  }

public:
  void reset(String initialStr = "", int maxLength = 30) {
    inputString = initialStr;
    if (maxLength > INPUT_BUFFER_CAPACITY - 1) maxLength = INPUT_BUFFER_CAPACITY - 1;
    maxLen = maxLength;
    inputString.reserve(maxLen + 1);
    if (inputString.length() > static_cast<size_t>(maxLen)) inputString.remove(maxLen);
    currentMode = 0;
    charSetLengths[3] = strlen(charSets[3]);
    for (int i = 0; i < 5; i++) focusIdx[i] = 0;
    dualTriggered = false;
    pendingBtn = 0;
    actionExecuted = false;
    cursorPos = inputString.length();
    isCursorMode = false;
    displayOffset = 0;
    carousel.reset();
    inputPulse=false;
  }

  const String& getInputString() const { return inputString; }
  bool consumePulse() { bool pending=inputPulse;inputPulse=false;return pending; }
  static oled::Rect selectorBox() { return {54,33,21,24}; }

  int processInput(bool okTriggered) {
    bool leftRaw = btnLeft.down();
    bool rightRaw = btnRight.down();
    unsigned long now = millis();

    if (leftRaw && rightRaw) {
      if (!dualTriggered && !isCursorMode) { 
        currentMode = (currentMode + 1) % 5;
        carousel.reset();
        dualTriggered = true;
        pendingBtn = 0;
        actionExecuted = true; 
      }
    } else if (leftRaw || rightRaw) {
      int activeBtn = leftRaw ? 1 : 2;
      if (!dualTriggered) {
        if (pendingBtn == 0) {
          pendingBtn = activeBtn;
          firstPressTime = now;
          actionExecuted = false;
          repeatDelay = inputSettings.repeatDelayMs;
        } else if (pendingBtn == activeBtn) {
          if (now - firstPressTime > DUAL_BUTTON_GRACE_MS && !actionExecuted) {
            executeScroll(pendingBtn);
            actionExecuted = true;
            lastRepeatTime = now;
          } else if (actionExecuted && now - lastRepeatTime > repeatDelay) {
            executeScroll(pendingBtn);
            lastRepeatTime = now;
            repeatDelay = 1000/inputSettings.repeatHz;
          }
        } else {
          pendingBtn = activeBtn;
          firstPressTime = now;
          actionExecuted = false;
          repeatDelay = inputSettings.repeatDelayMs;
        }
      }
    } else {
      if (pendingBtn != 0 && !actionExecuted && !dualTriggered) executeScroll(pendingBtn);
      pendingBtn = 0;
      dualTriggered = false;
      actionExecuted = false;
    }

    if (okTriggered) {
      inputPulse=isCursorMode || currentMode==4 || inputString.length()<maxLen;
      carousel.reset();
      if (isCursorMode) {
        isCursorMode = false;
      } else {
        if (currentMode == 4) {
          if (focusIdx[4] == 0 && cursorPos > 0) {
            inputString.remove(cursorPos - 1, 1);
            cursorPos--;
          }
          else if (focusIdx[4] == 1) return 1;
          else if (focusIdx[4] == 2) { inputString = ""; cursorPos = 0; }
          else if (focusIdx[4] == 3) return 2;
          else if (focusIdx[4] == 4) isCursorMode = true;
        } else { 
          if (inputString.length() < maxLen) {
            insertCharAtCursor(charSets[currentMode][focusIdx[currentMode]]);
          }
        }
      }
    }
    return 0;
  }

  void draw() {
    u8g2.drawFrame(5, 2, 118, 16);
    u8g2.setFont(u8g2_font_6x10_tr);

    size_t inputLen = inputString.length();
    if (cursorPos < 0) cursorPos = 0;
    if (static_cast<size_t>(cursorPos) > inputLen) cursorPos = static_cast<int>(inputLen);

    char textBeforeCursor[INPUT_BUFFER_CAPACITY];
    size_t prefixLen = static_cast<size_t>(cursorPos);
    if (prefixLen >= sizeof(textBeforeCursor)) prefixLen = sizeof(textBeforeCursor) - 1;
    memcpy(textBeforeCursor, inputString.c_str(), prefixLen);
    textBeforeCursor[prefixLen] = '\0';

    int cursorX = u8g2.getStrWidth(textBeforeCursor);
    int viewWidth = 106; 
    if (cursorX - displayOffset < 0) {
      displayOffset = cursorX - 10;
      if (displayOffset < 0) displayOffset = 0;
    } else if (cursorX - displayOffset > viewWidth) {
      displayOffset = cursorX - viewWidth;
    }

    u8g2.setClipWindow(6, 3, 121, 17);
    u8g2.setCursor(8 - displayOffset, 13);
    u8g2.print(inputString);

    if ((millis() / 300) % 2 == 0) {
      int cx = 8 - displayOffset + cursorX;
      if (isCursorMode) {
        u8g2.setDrawColor(2);
        u8g2.drawBox(cx, 4, 6, 11);
        u8g2.setDrawColor(1);
      } else {
        u8g2.drawLine(cx, 4, cx, 14);
      }
    }
    
    u8g2.setMaxClipWindow();

    if (isCursorMode && !inputPulse) {
      u8g2.setCursor(5, 28);
      u8g2.print("Mode: [ CURSOR ]");
      u8g2.setFont(u8g2_font_ncenB08_tr);
      int w = u8g2.getStrWidth("< Move Cursor >");
      u8g2.setCursor((128 - w) / 2, 45);
      u8g2.print("< Move Cursor >");
      u8g2.setFont(u8g2_font_6x10_tr);
      w = u8g2.getStrWidth("[OK] to Edit");
      u8g2.setCursor((128 - w) / 2, 60);
      u8g2.print("[OK] to Edit");
    } else {
      u8g2.setCursor(5, 28);
      const char* modeNames[] = { "[a-z]", "[A-Z]", "[0-9]", "[Sym]", "[CMD]" };
      u8g2.print("Mode: ");
      u8g2.print(modeNames[currentMode]);

      const int centerX=64, centerY=45;
      const oled::Rect box=selectorBox();
      const int left=box.x, right=box.x+box.w-1;
      int len=charSetLengths[currentMode], spacing=currentMode==4?45:24;
      float shift=carousel.offset(millis());
      // Each side of a wall keeps one blank column for glyph clearance.
      // The selection fill still reaches the frame; only text is clipped here.
      for(int zone=0;zone<3;zone++) {
        int x0=zone==0?0:zone==1?left+2:right+2;
        int x1=zone==0?left-1:zone==1?right-1:128;
        u8g2.setClipWindow(x0,box.y+2,x1,box.y+box.h-2);
        u8g2.setFont(zone==1&&currentMode!=4?u8g2_font_ncenB12_tr:u8g2_font_6x10_tr);
        for(int i=-3;i<=3;i++) {
          int target=(focusIdx[currentMode]+i+len)%len;
          char label[8];
          if(currentMode==4)copyToBuffer(label,sizeof(label),specialKeys[target]);
          else {label[0]=charSets[currentMode][target];label[1]=0;}
          int cx=centerX+oled::pixel((i+shift)*spacing);
          oled::centeredText(u8g2,{cx-box.w/2,box.y,box.w,box.h},centerY+(zone==1&&currentMode!=4?6:4),label);
        }
      }
      u8g2.setMaxClipWindow();oled::selection(u8g2,box,consumePulse());
    }
  }
} textInputMgr;

// The display manager owns all rendering, including the screensaver animation,
// menu marquee logic and modal loading overlays.
class DisplayManager {
private:
  oled::Motion motion;
  oled::Mask clockRaster, envRaster, previewRaster;
  oled::UiAnimation animation;
  oled::Rect focusRect={};
  bool focusRequested=false, focusFilled=false;
  oled::RollingText rolling;
  unsigned long rasterTime=0, motionTime=0;
  bool motionReady=false;
  bool motionPaused=true;
  int requestedWeather=-1;
  uint8_t screenBeforeFrame=255;
  int scrollView=-1,scrollTarget=0;
  float scrollFrom=0;
  unsigned long scrollStarted=0;

  float listOffset(int target) {
    unsigned long now=millis();
    int page=uiPage(state.uiState);
    if(scrollView!=page || state.isEditing || uiHold.running() || state.wifiHoldingOk){scrollView=page;scrollTarget=target;scrollFrom=target;scrollStarted=now;}
    float t=oled::easeOut(oled::limit((now-scrollStarted)/160.f,0,1));
    float value=scrollFrom+(scrollTarget-scrollFrom)*t;
    if(scrollTarget!=target){scrollFrom=value;scrollTarget=target;scrollStarted=now;}
    return value;
  }

  int uiDepth() const {
    switch(state.uiState) {
      case SCREEN_SAVER:return 0;
      case MAIN_MENU:return 1;
      case ALARM_LIST:case LOCATION_SETUP:case WIFI_LIST:case WIFI_SCANNING:case CO2_SETTING:case MOTION_SETTING:case INPUT_SETTING:return 2;
      case ALARM_EDIT:case WIFI_PASS_INPUT:case LOCATION_SEARCH_INPUT:return 3;
      case ALARM_DAY_SELECT:case ALARM_OPTIONS:case LOCATION_RESULT_LIST:return 4;
      case ALARM_SNOOZE:case ALARM_LIGHT:return 5;
      default:return 2;
    }
  }
  void queueFocus(int x,int y,int w,int h,bool filled=false) {
    focusRect={x,y,w,h};focusRequested=true;focusFilled=filled;
  }
  void finishFocus(unsigned long now) {
    if(!focusRequested)return;
    oled::Rect r=animation.focus(focusRect,now,state.isEditing || uiHold.running() || state.wifiHoldingOk);
    float progress=uiHold.running() && uiHold.key/1000==state.uiState ? uiHold.fraction(now) : 0;
    oled::selection(u8g2,r,focusFilled,progress);
    if(progress>=1)uiHold.presented(now);
  }
  void drawRaster(const oled::Body& b) {
    oled::Rect r=b.box();
    for(int y=0;y<b.mask.h;y++)for(int x=0;x<b.mask.w;x++)if(b.mask.lit(x,y))u8g2.drawPixel(r.x+x,r.y+y);
  }
  void rasterClock(oled::Mask& mask) {
    char timeText[8],dateText[16];getTimeDisplayText(timeText,sizeof(timeText));getDateDisplayText(dateText,sizeof(dateText));
    u8g2.clearBuffer();u8g2.setDrawColor(1);u8g2.setFont(u8g2_font_helvB14_tn);int tw=u8g2.getStrWidth(timeText);
    u8g2.setFont(u8g2_font_profont10_mr);int dw=u8g2.getStrWidth(dateText);int width=max(tw,dw);
    u8g2.setFont(u8g2_font_helvB14_tn);u8g2.drawStr(8+(width-tw)/2,29,timeText);
    u8g2.setFont(u8g2_font_profont10_mr);u8g2.drawStr(8+(width-dw)/2,37,dateText);
    mask.capture(u8g2.getBufferPtr());
  }
  void rasterEnvironment(oled::Mask& mask,int cycle) {
    u8g2.clearBuffer();u8g2.setDrawColor(1);drawCO2(8,24,state.co2);drawWeatherInfoRow(8,41,cycle);
    mask.capture(u8g2.getBufferPtr());
  }
  BouncingElement wakeUpElement = { 0.0f, 0.0f, 0.0f, 0.0f, 1, 1 };
  bool wakeUpElementInitialized = false;
  unsigned long wakeUpMotionTime = 0;
  unsigned long lastDrawTime = 0;
  uint8_t sentFrame[1024] = {};
  bool frameSent = false;
  int lastContrast = -1;
  uint32_t marqueeFrameSerial = 0;
  uint32_t marqueeLastSeenFrame = 0;
  int marqueeFocusKey = -1;
  unsigned long marqueeFocusStartTime = 0;

  float readBounceJitterRadians() {
    const float maxJitterRadians = 3.0f * (PI / 180.0f);
    float unit = ((esp_random() >> 8) & 0x00FFFFFF) / 16777215.0f;
    return (unit * 2.0f - 1.0f) * maxJitterRadians;
  }

  void rescaleVelocity(float& dirX, float& dirY, float targetSpeed) {
    float speed = sqrtf(dirX * dirX + dirY * dirY);
    if (speed < 0.0001f) {
      dirX = targetSpeed;
      dirY = 0.0f;
      return;
    }

    float scale = targetSpeed / speed;
    dirX *= scale;
    dirY *= scale;
  }

  void applyBounceJitter(BouncingElement& e, int xSignLock = 0, int ySignLock = 0) {
    float targetSpeed = sqrtf(e.dirX * e.dirX + e.dirY * e.dirY);
    float jitter = readBounceJitterRadians();
    float cosTheta = cosf(jitter);
    float sinTheta = sinf(jitter);
    float nextX = e.dirX * cosTheta - e.dirY * sinTheta;
    float nextY = e.dirX * sinTheta + e.dirY * cosTheta;

    if (xSignLock > 0) nextX = fabsf(nextX);
    else if (xSignLock < 0) nextX = -fabsf(nextX);

    if (ySignLock > 0) nextY = fabsf(nextY);
    else if (ySignLock < 0) nextY = -fabsf(nextY);

    e.dirX = nextX;
    e.dirY = nextY;
    e.enforceMinimumSpeed();
    rescaleVelocity(e.dirX, e.dirY, targetSpeed);
  }

  void initializeWakeUpElement() {
    static const char* wakeText = "WAKE UP!";
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.clearBuffer();u8g2.drawStr(8,24,wakeText);previewRaster.capture(u8g2.getBufferPtr());u8g2.clearBuffer();
    wakeUpElement.w = previewRaster.w;
    wakeUpElement.h = previewRaster.h;
    wakeUpElement.x = (SCREEN_W - wakeUpElement.w) / 2.0f;
    wakeUpElement.y = (SCREEN_H - wakeUpElement.h) / 2.0f;

    // Exactly twice the clock's configured pixels-per-second speed.
    const float clockSpeed = motionSettings.speedHalf*.5f;
    const float wakeSpeed = clockSpeed * 2.0f;
    float angle = (esp_random() / 4294967295.0f) * 2.0f * PI;
    wakeUpElement.dirX = cosf(angle) * wakeSpeed;
    wakeUpElement.dirY = sinf(angle) * wakeSpeed;
    wakeUpElement.enforceMinimumSpeed();
    rescaleVelocity(wakeUpElement.dirX, wakeUpElement.dirY, wakeSpeed);
    wakeUpElementInitialized = true;
    wakeUpMotionTime=millis();
  }

  void updateWakeUpElement() {
    const float maxX = max(0, SCREEN_W - wakeUpElement.w);
    const float maxY = max(0, SCREEN_H - wakeUpElement.h);
    unsigned long now=millis();float dt=min((now-wakeUpMotionTime)/1000.f,.1f);wakeUpMotionTime=now;
    float nextX = wakeUpElement.x + wakeUpElement.dirX*dt;
    float nextY = wakeUpElement.y + wakeUpElement.dirY*dt;
    int xSignLock = 0;
    int ySignLock = 0;

    // Reflect the fractional overshoot at the real glyph box boundary. This
    // prevents both early bounces and a rounded draw position outside the OLED.
    if (nextX < 0.0f) {
      nextX = -nextX;
      wakeUpElement.dirX = fabsf(wakeUpElement.dirX);
      xSignLock = 1;
    } else if (nextX > maxX) {
      nextX = maxX - (nextX - maxX);
      wakeUpElement.dirX = -fabsf(wakeUpElement.dirX);
      xSignLock = -1;
    }

    if (nextY < 0.0f) {
      nextY = -nextY;
      wakeUpElement.dirY = fabsf(wakeUpElement.dirY);
      ySignLock = 1;
    } else if (nextY > maxY) {
      nextY = maxY - (nextY - maxY);
      wakeUpElement.dirY = -fabsf(wakeUpElement.dirY);
      ySignLock = -1;
    }

    wakeUpElement.x = constrain(nextX, 0.0f, maxX);
    wakeUpElement.y = constrain(nextY, 0.0f, maxY);
    if (xSignLock != 0 || ySignLock != 0) {
      applyBounceJitter(wakeUpElement, xSignLock, ySignLock);
    }
  }

  void drawTimeAlarm() {
    if(alarmMgr.checkRinging()) {
      drawPanelHeader("Wake Check");
      u8g2.setFont(u8g2_font_ncenB08_tr);
      const char* label="Still awake?";
      u8g2.drawStr((128-u8g2.getStrWidth(label))/2,38,label);
      return;
    }
    static const char* wakeText = "WAKE UP!";
    if (millis() - state.alarmStartTime < ALARM_STATIC_DISPLAY_MS) {
      wakeUpElementInitialized = false;
      u8g2.setFont(u8g2_font_ncenB14_tr);
      int w = u8g2.getStrWidth(wakeText);
      u8g2.setCursor((SCREEN_W - w) / 2, 40);
      u8g2.print(wakeText);
      if ((millis() / 500) % 2) u8g2.drawFrame(0, 0, SCREEN_W, SCREEN_H);
      return;
    }

    if (!wakeUpElementInitialized) initializeWakeUpElement();
    else updateWakeUpElement();

    int x=wakeUpElement.drawX(),y=wakeUpElement.drawY();
    for(int yy=0;yy<previewRaster.h;yy++)for(int xx=0;xx<previewRaster.w;xx++)if(previewRaster.lit(xx,yy))u8g2.drawPixel(x+xx,y+yy);
  }

  void drawMenuCloseWarning() {
    if (state.uiState == SCREEN_SAVER || alarmOutputController.isTimeAlarmActive() || loadingOverlay.active) return;

    unsigned long elapsed = millis() - state.lastInputTime;
    const unsigned long warningStart = MENU_INACTIVITY_TIMEOUT_MS - MENU_INACTIVITY_WARNING_MS;
    if (elapsed < warningStart || elapsed >= MENU_INACTIVITY_TIMEOUT_MS) return;

    unsigned long remainingMs = MENU_INACTIVITY_TIMEOUT_MS - elapsed;
    unsigned int countdown = (remainingMs + 999UL) / 1000UL;
    char label[20];
    snprintf(label, sizeof(label), "Close Menu: %02u", countdown);

    u8g2.setFont(u8g2_font_6x10_tr);
    const int border = 1;
    const int margin = 1;
    const int inset = border + margin;
    int textW = u8g2.getStrWidth(label);
    int ascent = u8g2.getAscent();
    int textH = max(1, ascent - u8g2.getDescent());
    int boxW = textW + inset * 2;
    int boxH = textH + inset * 2;
    int boxX = (SCREEN_W - boxW) / 2;
    int boxY = (SCREEN_H - boxH) / 2;

    u8g2.setDrawColor(0);
    u8g2.drawBox(boxX, boxY, boxW, boxH);
    u8g2.setDrawColor(1);
    u8g2.drawFrame(boxX, boxY, boxW, boxH);
    u8g2.setCursor(boxX + inset, boxY + inset + ascent);
    u8g2.print(label);
  }

  void getTempDisplayText(char* dest, size_t destSize) {
    if (!state.isWeatherSynced) {
      copyToBuffer(dest, destSize, "--.- C");
      return;
    }
    snprintf(dest, destSize, "%.1f C", state.tempOut);
  }

  void getRainDisplayText(char* dest, size_t destSize) {
    if (!state.isWeatherSynced) {
      copyToBuffer(dest, destSize, "--%");
      return;
    }
    snprintf(dest, destSize, "%d%%", state.rainProb);
  }

  void getWindDisplayText(char* dest, size_t destSize) {
    if (!state.isWeatherSynced) {
      copyToBuffer(dest, destSize, "--.-km/h");
      return;
    }
    snprintf(dest, destSize, "%.1fkm/h", state.windSpeed);
  }

  void getPmDisplayText(char* dest, size_t destSize) {
    if (!state.isWeatherSynced) {
      copyToBuffer(dest, destSize, "--");
      return;
    }
    snprintf(dest, destSize, "%d", static_cast<int>(state.pm10));
  }

  void getLocationSummaryLine(char* dest, size_t destSize) {
    if (!hasValidLatLon(state.lat, state.lon)) {
      copyToBuffer(dest, destSize, "No city");
      return;
    }

    copyToBuffer(dest, destSize, state.cityName[0] != '\0' ? state.cityName : "Unnamed location");
  }

  void getTimeDisplayText(char* dest, size_t destSize) {
    if (!state.isTimeSynced) {
      copyToBuffer(dest, destSize, "--:--");
      return;
    }

    struct tm t = rtc.getTimeStruct();
    snprintf(dest, destSize, "%02d:%02d", t.tm_hour, t.tm_min);
  }

  void getDateDisplayText(char* dest, size_t destSize) {
    if (!state.isTimeSynced) {
      copyToBuffer(dest, destSize, "--/-- ---");
      return;
    }

    static const char* kWeekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct tm t = rtc.getTimeStruct();
    int weekday = clampValue(t.tm_wday, 0, 6);
    snprintf(dest, destSize, "%02d/%02d %s", t.tm_mon + 1, t.tm_mday, kWeekdays[weekday]);
  }

  void buildLocationResultLabel(const LocationResult& res, char* dest, size_t destSize) {
    if (res.country.length() > 0) {
      snprintf(dest, destSize, "%s, %s", res.name.c_str(), res.country.c_str());
    } else {
      copyToBuffer(dest, destSize, res.name);
    }
  }

  void ellipsizeTextInto(const char* text, int maxWidth, char* out, size_t outSize) {
    copyToBuffer(out, outSize, text ? text : "");
    if (u8g2.getStrWidth(out) <= maxWidth) return;

    size_t len = strlen(out);
    while (len > 0) {
      out[--len] = '\0';
      if (len + 2 >= outSize) continue;
      out[len] = '.';
      out[len + 1] = '.';
      out[len + 2] = '\0';
      if (u8g2.getStrWidth(out) <= maxWidth) {
        return;
      }
      out[len] = '\0';
    }

    out[0] = '\0';
  }

  uint16_t getWeatherIcon(int code, int isDay) {
    if (code <= 1) return isDay ? 57908 : 57907;
    if (code == 2) return isDay ? 57905 : 57906;
    if (code == 3) return 57899;
    if (code >= 45 && code <= 48) return 58079;
    if (code >= 51 && code <= 67) return 57901;
    if (code >= 71 && code <= 77) return 57900;
    if (code >= 80 && code <= 82) return 57901;
    if (code >= 85 && code <= 86) return 57900;
    if (code >= 95) return 57383;
    return 57548;
  }

  uint16_t getArrowIcon(int deg) {
    int idx = (int)((deg + 22.5) / 45.0) % 8;
    const uint16_t arrows[] = { 11014, 11016, 10145, 11018, 11015, 11019, 11013, 11017 };
    return arrows[idx];
  }

  void drawCO2(int x, int y, int value) {
    u8g2.setFont(u8g2_font_helvB08_tr);
    u8g2.setCursor(x, y);
    u8g2.print("CO");
    int w = u8g2.getStrWidth("CO");
    u8g2.setFont(u8g2_font_u8glib_4_tf);
    u8g2.setCursor(x + w, y + 2);
    u8g2.print("2");
    u8g2.setFont(u8g2_font_helvB08_tr);
    u8g2.setCursor(x + w + 4, y);
    if(state.isSensorError){u8g2.print(": Er");return;}
    char valueText[16];
    snprintf(valueText, sizeof(valueText), ": %d", value);
    u8g2.print(valueText);
    int valW = u8g2.getStrWidth(valueText);
    u8g2.setCursor(x + w + 4 + valW + 1, y);
    u8g2.setFont(u8g2_font_profont10_mr);
    u8g2.print("ppm");
  }

  void drawWeatherInfoRow(int x, int y, int infoCycle) {
    char text[20];

    if (infoCycle == 0) {
      u8g2.setFont(u8g2_font_waffle_t_all);
      u8g2.drawGlyph(x, y - 5, getWeatherIcon(state.weatherCode, state.isDay));
      u8g2.setFont(u8g2_font_helvB08_te);
      u8g2.setCursor(x + 18, y - 4);
      getTempDisplayText(text, sizeof(text));
      char* unit=strrchr(text,' ');if(unit)*unit=0;
      u8g2.print(text);int degreeX=x+18+u8g2.getStrWidth(text)+1;
      u8g2.drawFrame(degreeX,y-12,3,3);u8g2.drawStr(degreeX+4,y-4,"C");
      return;
    }

    if (infoCycle == 1) {
      u8g2.setFont(u8g2_font_waffle_t_all);
      u8g2.drawGlyph(x, y - 4, 58128);
      u8g2.setFont(u8g2_font_helvB08_tr);
      u8g2.setCursor(x + 18, y - 4);
      getRainDisplayText(text, sizeof(text));
      u8g2.print(text);
      return;
    }

    if (infoCycle == 2) {
      u8g2.setFont(u8g2_font_unifont_t_75);
      u8g2.drawGlyph(x, y - 4, 9641);
      u8g2.setFont(u8g2_font_helvB08_tr);
      u8g2.setCursor(x + 18, y - 4);
      getPmDisplayText(text, sizeof(text));
      u8g2.print(text);
      u8g2.print("ug/m");
      int baseTextWidth = u8g2.getStrWidth(text) + u8g2.getStrWidth("ug/m");
      u8g2.setFont(u8g2_font_u8glib_4_tf);
      u8g2.drawStr(x + 18 + baseTextWidth + 1, y - 8, "3");
      return;
    }

    uint16_t arrowCode = getArrowIcon(state.windDir);
    if (arrowCode == 10145) u8g2.setFont(u8g2_font_unifont_t_78_79);
    else u8g2.setFont(u8g2_font_unifont_t_86);
    u8g2.drawGlyph(x, y - 3, arrowCode);
    u8g2.setFont(u8g2_font_helvB08_tr);
    u8g2.setCursor(x + 18, y - 4);
    getWindDisplayText(text, sizeof(text));
    u8g2.print(text);
  }

  int getFocusedMarqueeOffset(int focusKey, int contentWidth, int viewWidth) {
    if (contentWidth <= viewWidth) return 0;

    if (marqueeFocusKey != focusKey || marqueeLastSeenFrame + 1 < marqueeFrameSerial) {
      marqueeFocusKey = focusKey;
      marqueeFocusStartTime = millis();
    }
    marqueeLastSeenFrame = marqueeFrameSerial;

    const unsigned long holdStartMs = 900;
    const unsigned long holdEndMs = holdStartMs;
    const unsigned long pxStepMs = 35;
    const int travel = contentWidth - viewWidth;
    const unsigned long travelMs = static_cast<unsigned long>(travel) * pxStepMs;

    unsigned long elapsed = millis() - marqueeFocusStartTime;
    if (elapsed < holdStartMs) return 0;
    elapsed -= holdStartMs;

    if (elapsed < travelMs) return static_cast<int>(elapsed / pxStepMs);
    elapsed -= travelMs;

    if (elapsed < holdEndMs) return travel;

    marqueeFocusStartTime = millis();
    return 0;
  }

  void drawScrollableText(int x, int baselineY, int viewWidth, const char* text, bool isFocused, int focusKey) {
    const char* safeText = text ? text : "";
    int textWidth = u8g2.getStrWidth(safeText);
    if (!isFocused) {
      if (textWidth <= viewWidth) {
        u8g2.setCursor(x, baselineY);
        u8g2.print(safeText);
      } else {
        char clipped[160];
        ellipsizeTextInto(safeText, viewWidth, clipped, sizeof(clipped));
        u8g2.setCursor(x, baselineY);
        u8g2.print(clipped);
      }
      return;
    }

    if (textWidth <= viewWidth) {
      u8g2.setCursor(x, baselineY);
      u8g2.print(safeText);
      return;
    }

    int offset = getFocusedMarqueeOffset(focusKey, textWidth, viewWidth);
    u8g2.setClipWindow(x, baselineY - 10, x + viewWidth, baselineY + 2);
    u8g2.setCursor(x - offset, baselineY);
    u8g2.print(safeText);
    u8g2.setMaxClipWindow();
  }

  void drawScrollableText(int x, int baselineY, int viewWidth, const String& text, bool isFocused, int focusKey) {
    drawScrollableText(x, baselineY, viewWidth, text.c_str(), isFocused, focusKey);
  }

  void drawFocus(int row, int targetRow, int x, int y, int w, int h) {
    if (row == targetRow) {
      queueFocus(x,y,w,h,state.isEditing);
    }
    u8g2.setDrawColor(1);
  }

  void drawPanelHeader(const char* title) {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    int titleW = u8g2.getStrWidth(title);
    u8g2.setCursor((128 - titleW) / 2, 11);
    u8g2.print(title);
    u8g2.drawLine(0, 14, 128, 14);
  }

  void drawConfirmButtons(bool yesSelected, int baselineY = 61) {
    const int boxY = baselineY - 10;
    oled::Rect no={16,boxY,34,12},yes={74,boxY,38,12};
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setDrawColor(1);oled::centeredText(u8g2,no,baselineY,"No");oled::centeredText(u8g2,yes,baselineY,"Yes");
    oled::Rect selected=yesSelected?yes:no;queueFocus(selected.x,selected.y,selected.w,selected.h);
  }

public:
  void init() {
    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setPowerSave(0);
    u8g2.setContrast(255);
    u8g2.setFontMode(1);
    u8g2.clearDisplay();
  }

  void drawSpinner(int cx, int cy) {
    int frame = (millis() / 60) % 8;
    const int8_t dx[8] = {0, 3, 4, 3, 0, -3, -4, -3};
    const int8_t dy[8] = {-4, -3, 0, 3, 4, 3, 0, -3};
    u8g2.drawLine(cx, cy, cx + dx[frame], cy + dy[frame]);
    u8g2.drawPixel(cx + dx[(frame + 7) % 8], cy + dy[(frame + 7) % 8]);
    u8g2.drawPixel(cx + dx[(frame + 6) % 8], cy + dy[(frame + 6) % 8]);
  }

  void drawLoading(const char* text,int y=40,bool showSpinner=false) {
    const char* safeText=text?text:"";
    u8g2.setFont(u8g2_font_helvB08_tr);
    char line1[96],line2[96],clipped[96];splitTextOnNewline(safeText,line1,sizeof(line1),line2,sizeof(line2));
    bool spinner=showSpinner||loadingTextRequestsSpinner(safeText);
    ellipsizeTextInto(line1,124,clipped,sizeof(clipped));
    u8g2.drawStr((128-u8g2.getStrWidth(clipped))/2,line2[0]?y-12:y,clipped);
    if(line2[0]){ellipsizeTextInto(line2,124,clipped,sizeof(clipped));u8g2.drawStr((128-u8g2.getStrWidth(clipped))/2,y+4,clipped);}
    if(spinner)drawSpinner(64,58);
  }

  // Rendering always starts from a clean draw state so no stale clip or color state leaks between frames.
  void updateAndDraw() {
    if (millis() - lastDrawTime < FRAME_DELAY) return;
    lastDrawTime = millis();
    if(state.uiState!=SCREEN_SAVER)motionPaused=true;
    marqueeFrameSerial++;
    bool urgent=alarmOutputController.isTimeAlarmActive() || alarmMgr.missionActive() || loadingOverlay.active;
    int frameView=urgent ? (alarmMgr.missionActive()?102:alarmOutputController.isTimeAlarmActive()?100:101) : uiPage(state.uiState);
    if(alarmMgr.missionActive() || alarmOutputController.isTimeAlarmActive()){animation.cancelActivation();textInputMgr.consumePulse();}
    else if(loadingOverlay.active && textInputMgr.consumePulse())animation.activate(TextInputManager::selectorBox());
    if(screenBeforeFrame!=static_cast<uint8_t>(state.uiState) || (!state.isEditing && state.uiState!=CO2_SETTING))rolling.reset();
    animation.begin(frameView,uiDepth(),lastDrawTime,!urgent && screenBeforeFrame<100);
    screenBeforeFrame=urgent?100:static_cast<uint8_t>(state.uiState);
    focusRequested=false;

    if(lastContrast!=state.displayContrast){u8g2.setContrast(state.displayContrast);lastContrast=state.displayContrast;}
    u8g2.setDrawColor(1);
    u8g2.setMaxClipWindow();
    u8g2.clearBuffer();

    if (alarmMgr.missionActive()) {
      drawAlarmMission();
    } else if (alarmOutputController.isTimeAlarmActive()) {
      drawTimeAlarm();
    } else if (loadingOverlay.active) {
      wakeUpElementInitialized = false;
      drawLoading(loadingOverlay.message, loadingOverlay.y, loadingOverlay.showSpinner);
    } else {
      wakeUpElementInitialized = false;
      switch (state.uiState) {
        case SCREEN_SAVER: drawScreensaver(); break;
        case MAIN_MENU: drawMainMenu(); break;
        case ALARM_LIST: drawAlarmList(); break;
        case ALARM_EDIT: drawAlarmEdit(); break;
        case ALARM_DAY_SELECT: drawDaySelect(); break;
        case ALARM_OPTIONS: drawAlarmOptions(); break;
        case ALARM_SNOOZE: drawSnooze(); break;
        case ALARM_LIGHT: drawLightLearning(); break;
        case MOTION_SETTING: drawMotionSettings(); break;
        case INPUT_SETTING: drawInputSettings(); break;
        case CALIBRATION_CONFIRM: drawCalibration(); break;
        case ABC_CONFIRM: drawAbcConfirm(); break;
        case LOCATION_SETUP: drawLocation(); break;
        case CO2_SETTING: drawCo2Setting(); break;
        case WIFI_SCANNING: drawWiFiScanning(); break;
        case WIFI_LIST:
        case WIFI_CONNECTING: drawWiFiList(); break;
        case WIFI_PASS_INPUT: textInputMgr.draw(); break;
        case LOCATION_SEARCH_INPUT: textInputMgr.draw(); break;
        case LOCATION_RESULT_LIST: drawLocationResultList(); break;
        case RESET_CONFIRM: drawResetConfirm(); break;
      }
      finishFocus(lastDrawTime);
      drawMenuCloseWarning();
    }
    if(uiHold.running() && uiHold.key==-2) {
      oled::selection(u8g2,{1,1,126,62},false,uiHold.fraction(lastDrawTime));
      uiHold.presented(lastDrawTime);
    }
    animation.compose(u8g2.getBufferPtr(),lastDrawTime);
    oled::presentChanged(u8g2,sentFrame,frameSent);
  }

  void navigationActivated(UIState before) {
    if((before==WIFI_PASS_INPUT || before==LOCATION_SEARCH_INPUT) && textInputMgr.consumePulse())animation.activate(TextInputManager::selectorBox());
    else animation.activate();
  }

  void drawScreensaver() {
    const unsigned long now=millis();motion.settings=motionSettings;
    if(motionPaused&&motionReady){motion.resume();motionTime=now;}motionPaused=false;
    if(!motionReady || now-rasterTime>=200 || requestedWeather!=state.weatherInfoCycle) {
      rasterTime=now;rasterClock(clockRaster);
      unsigned long period=motionSettings.cycleSeconds*1000UL;
      bool preview=motionReady && !state.weatherTransitionPending && now-state.lastWeatherInfoCycleChange>=period-1000;
      int phase=preview?(state.weatherInfoCycle+1)%WEATHER_INFO_COUNT:state.weatherInfoCycle;
      rasterEnvironment(envRaster,phase);
      if(!motionReady) {
        motion.seed(esp_random());motion.initialize(clockRaster,envRaster);motionReady=true;
      } else {motion.offer(0,clockRaster);motion.offer(1,envRaster,!preview);}
      requestedWeather=state.weatherInfoCycle;
    }
    float seconds=motionTime ? min((now-motionTime)/1000.f,.1f) : 0;
    motionTime=now;motion.advance(seconds);
    if(state.weatherTransitionPending && !motion.bodies[1].pending) {
      state.weatherTransitionPending=false;state.lastWeatherInfoCycleChange=now;
    }
    u8g2.clearBuffer();drawRaster(motion.bodies[0]);drawRaster(motion.bodies[1]);
  }

  void drawMainMenu() {
    const char* menus[]={"< Back","Alarms","CO2 Alert Setup","ABC Setup","Manual Calibration","Location Setup","WiFi Setup","Motion Setup","Input Setup","Full Reset"};
    float offset=listOffset(state.menuOffset);
    for(int item=floorf(offset);item<static_cast<int>(ceilf(offset))+4 && item<MAIN_ITEMS;item++) {
      int y=14+oled::pixel((item-offset)*14);
      u8g2.setFont(u8g2_font_ncenB08_tr);u8g2.setDrawColor(1);
      if(item==2) drawCo2Label(5,y," Alert Setup");
      else u8g2.drawStr(5,y,menus[item]);
      if(item==state.menuIdx)queueFocus(0,clampValue(y-10,0,50),128,14);
    }
  }

  void drawResetConfirm() {
    drawPanelHeader("Full Reset");
    u8g2.setFont(u8g2_font_6x10_tr);
    if (state.resetConfirmStep == 0) {
      u8g2.setCursor(1, 31);
      u8g2.print("Erase all saved data?");
      u8g2.setCursor(1, 42);
      u8g2.print("All data will be lost");
    } else {
      u8g2.setCursor(6, 31);
      u8g2.print("Cannot undo this!");
      u8g2.setCursor(6, 42);
      u8g2.print("Reset device now?");
    }
    drawConfirmButtons(state.resetConfirmYes);
  }

  void drawAlarmList() {
    u8g2.setFont(u8g2_font_6x10_tr);
    int totalItems = alarmMgr.alarmCount + 2;
    clampListState(totalItems);
    float offset=listOffset(state.listOffset);
    for (int cIdx=floorf(offset); cIdx<static_cast<int>(ceilf(offset))+5; cIdx++) {
      if (cIdx >= totalItems) break;
      int y = 12 + oled::pixel((cIdx-offset)*12);
      if (cIdx == state.listIdx) {
        queueFocus(0, clampValue(y-9,0,51), 128, 13);
      } else u8g2.setDrawColor(1);

      u8g2.setCursor(4, y);
      if (cIdx == 0) u8g2.print("< Back");
      else if (cIdx == totalItems - 1) u8g2.print("+ Add New Alarm");
      else {
        Alarm& a = alarmMgr.alarms[cIdx - 1];
        u8g2.printf("%02d:%02d ", a.hour, a.minute);
        if (a.days == 0) u8g2.print("Once");
        else if (a.days == 127) u8g2.print("Daily");
        else u8g2.print("Days");
        u8g2.print(a.enabled ? " [On]" : " [Off]");
      }
    }
    u8g2.setDrawColor(1);
  }

  void drawAlarmEdit() {
    drawPanelHeader("Edit Alarm");
    char digits[4];
    u8g2.setFont(u8g2_font_6x10_tr);
    drawFocus(state.editRow,0,10,19,34,11);
    oled::centeredText(u8g2,{10,19,34,11},28,state.tempAlarm.enabled?"On":"Off");
    u8g2.setFont(u8g2_font_ncenB12_tr);
    drawFocus(state.editRow,1,52,16,28,16);snprintf(digits,sizeof(digits),"%02u",state.tempAlarm.hour);
    if(state.isEditing&&state.editRow==1)rolling.draw(u8g2,ALARM_EDIT*100+1,digits,56,30,{54,18,24,12},state.valueDirection,millis(),true);
    else u8g2.drawStr(56,30,digits);
    u8g2.drawStr(81,29,":");
    drawFocus(state.editRow,2,86,16,28,16);snprintf(digits,sizeof(digits),"%02u",state.tempAlarm.minute);
    if(state.isEditing&&state.editRow==2)rolling.draw(u8g2,ALARM_EDIT*100+2,digits,90,30,{88,18,24,12},state.valueDirection,millis(),true);
    else u8g2.drawStr(90,30,digits);
    u8g2.setFont(u8g2_font_6x10_tr);
    drawFocus(state.editRow,3,2,35,82,13);u8g2.setCursor(5,44);
    u8g2.print(state.tempAlarm.days==0?"Repeat:Once>":state.tempAlarm.days==127?"Repeat:Daily>":"Repeat:Days>");
    drawFocus(state.editRow,4,86,35,40,11);oled::centeredText(u8g2,{86,35,40,11},44,"More >");
    drawFocus(state.editRow,5,2,50,40,11);oled::centeredText(u8g2,{2,50,40,11},59,"Cancel");
    drawFocus(state.editRow,6,46,50,32,11);oled::centeredText(u8g2,{46,50,32,11},59,"Save");
    drawFocus(state.editRow,7,82,50,44,11);oled::centeredText(u8g2,{82,50,44,11},59,"Delete");
  }

  void drawDaySelect() {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    int titleW = u8g2.getStrWidth("Day Select");
    u8g2.setCursor((128 - titleW) / 2, 11);
    u8g2.print("Day Select");
    u8g2.drawLine(0, 14, 128, 14);

    u8g2.setFont(u8g2_font_6x10_tr);
    const char* dayNames[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    for (int i = 0; i < 8; i++) {
      int col = i % 4, row = i / 4;
      int x = 2 + col * 32, y = 32 + row * 22;
      bool isSelected = i < 7 && ((state.tempAlarm.days >> i) & 1);
      if (isSelected) {
        u8g2.setDrawColor(1); u8g2.drawBox(x + 2, y - 8, 26, 10); u8g2.setDrawColor(0);
      } else u8g2.setDrawColor(1);

      u8g2.setCursor(x + 4, y);
      if (i == 7) u8g2.print("Done");
      else u8g2.print(dayNames[i]);
      u8g2.setDrawColor(1);

      if (state.daySelectIdx == i) queueFocus(x, y - 10, 30, 14);
    }
  }

  void drawCalibration() {
    drawPanelHeader("Manual Calibration");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(6, 26);
    u8g2.printf("Current: %d ppm", state.co2);
    u8g2.setCursor(6, 37);
    u8g2.print("Start calibrate?");
    u8g2.setCursor(6, 48);
    u8g2.print("in fresh air only!");
    drawConfirmButtons(state.calConfirmYes);
  }

  void drawAbcConfirm() {
    drawPanelHeader(state.abcEnabled?"ABC [ON]":"ABC [OFF]");
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(15,37);u8g2.print("Turn ABC ");
    u8g2.print(state.abcConfirmTargetOn?"On now?":"Off now?");
    drawConfirmButtons(state.abcConfirmYes);
  }

  void drawLocation() {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    int titleW = u8g2.getStrWidth("Location Setup");
    u8g2.setCursor((128 - titleW) / 2, 11);
    u8g2.print("Location Setup");
    u8g2.drawLine(0, 14, 128, 14);

    u8g2.setFont(u8g2_font_6x10_tr);
    char summary[160];
    getLocationSummaryLine(summary, sizeof(summary));
    drawScrollableText(2, 24, 124, summary, false, 0);
    u8g2.drawLine(0, 27, 128, 27);

    const int itemCount = 3;

    int visibleRows = 3;
    int offset = 0;
    if (state.locMenuIdx >= visibleRows) offset = state.locMenuIdx - visibleRows + 1;
    if (offset > itemCount - visibleRows) offset = max(0, itemCount - visibleRows);

    for (int row = 0; row < visibleRows; row++) {
      int i = offset + row;
      if (i >= itemCount) break;
      int y = 37 + row * 12;
      if (i == state.locMenuIdx) {
        queueFocus(0, y - 9, 128, row==visibleRows-1?12:13); u8g2.setDrawColor(1);
      } else u8g2.setDrawColor(1);

      if (i == 0) {
        u8g2.setCursor(4, y);
        u8g2.print("< Back");
      } else {
        u8g2.setCursor(4, y);
        u8g2.print((i == 1) ? "Search City" : "Clear Location");
      }

      u8g2.setDrawColor(1);
    }
  }

  void drawOptionRow(int row,int selected,const char* label) {
    int y=12+row*12;
    u8g2.setFont(u8g2_font_6x10_tr);u8g2.setDrawColor(1);u8g2.drawStr(4,y,label);
    if(row==selected)queueFocus(0,y-9,128,13,state.isEditing);
  }

  void drawSettingRow(int item,int selected,float offset,const char* name,const char* value,bool numeric) {
    int y=12+oled::pixel((item-offset)*12);
    u8g2.setFont(u8g2_font_6x10_tr);u8g2.setDrawColor(1);u8g2.drawStr(4,y,name);
    const int cell=6; // This row uses the fixed-advance 6x10 font.
    int x=4+(strlen(name)+1)*cell;
    if(value[0]) {
      // The delimiters stay outside the animated value's clip window.
      int valueX=x+cell, valueWidth=strlen(value)*cell;
      u8g2.drawStr(x,y,"[");u8g2.drawStr(valueX+valueWidth,y,"]");
      if(y>=9 && y<=60 && numeric && item==selected && state.isEditing)
        rolling.draw(u8g2,static_cast<int>(state.uiState)*100+item,value,valueX,y,{valueX,y-7,valueWidth,9},state.valueDirection,millis(),true);
      else u8g2.drawStr(valueX,y,value);
    }
    if(item==selected)queueFocus(0,clampValue(y-9,0,51),128,13,state.isEditing);
  }

  void drawCo2Label(int x,int y,const char* suffix) {
    u8g2.setFont(u8g2_font_ncenB08_tr);u8g2.drawStr(x,y,"CO");
    int w=u8g2.getStrWidth("CO");
    u8g2.setFont(u8g2_font_u8glib_4_tf);u8g2.drawStr(x+w,y+1,"2");
    u8g2.setFont(u8g2_font_ncenB08_tr);u8g2.drawStr(x+w+4,y,suffix);
  }

  void drawAlarmOptions() {
    const AlarmOptions& o=state.tempAlarmOptions;
    float offset=listOffset(state.optionOffset);
    for(int item=floorf(offset);item<static_cast<int>(ceilf(offset))+5 && item<ALARM_OPTION_ITEMS;item++) {
      char label[24]={},value[12]={};bool numeric=false;
      switch(item) {
        case 0:copyToBuffer(label,sizeof(label),"< Back");break;
        case 1:snprintf(label,sizeof(label),"Snooze [%um x%u] >",o.snoozeMinutes,o.snoozeCount);break;
        case 2:
          copyToBuffer(label,sizeof(label),"Wake Check");numeric=true;
          if(o.checkMinutes)snprintf(value,sizeof(value),"%2um",o.checkMinutes);else copyToBuffer(value,sizeof(value),"Off");break;
        case 3:copyToBuffer(label,sizeof(label),"Check Limit");snprintf(value,sizeof(value),"%3us",o.checkTimeoutSec);numeric=true;break;
        case 4: {
          static const char* names[]={"None","Keys","Light","Light+Keys"};
          snprintf(label,sizeof(label),"Mission [%s]",names[o.mission&3]);break;
        }
        default:
          if(o.lightRaw<=4095)snprintf(label,sizeof(label),"Light [%u%%] >",(4095-o.lightRaw)*100/4095);else copyToBuffer(label,sizeof(label),"Light [Learn] >");
      }
      drawSettingRow(item,state.optionIdx,offset,label,value,numeric);
    }
  }

  void drawSnooze() {
    drawOptionRow(0,state.snoozeIdx,"< Back");char value[16];
    snprintf(value,sizeof(value),"%2u min",state.tempAlarmOptions.snoozeMinutes);
    drawSettingRow(1,state.snoozeIdx,0,"Interval",value,true);
    snprintf(value,sizeof(value),"%u",state.tempAlarmOptions.snoozeCount);
    drawSettingRow(2,state.snoozeIdx,0,"Max Times",value,true);
  }

  void drawLightLearning() {
    drawPanelHeader("Light Mission");u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(7,28,"Current:");u8g2.setCursor(91,28);u8g2.printf("%d%%",(4095-state.filteredCds)*100/4095);
    u8g2.drawStr(7,42,"Learned:");u8g2.setCursor(91,42);
    if(state.tempAlarmOptions.lightRaw<=4095)u8g2.printf("%u%%",(4095-state.tempAlarmOptions.lightRaw)*100/4095);else u8g2.print("--");
    oled::centeredText(u8g2,{10,51,42,11},60,"Back");oled::centeredText(u8g2,{74,51,48,11},60,state.lightLearning?"Wait":"Learn");
    queueFocus(state.lightIdx?74:10,51,state.lightIdx?48:42,11,state.lightLearning);
    if(state.lightLearning)u8g2.drawHLine(7,46,min(114UL,(millis()-state.lightLearnStart)*114/1800));
  }

  void drawMotionSettings() {
    float offset=listOffset(state.motionOffset);char value[20];
    for(int item=floorf(offset);item<static_cast<int>(ceilf(offset))+5 && item<MOTION_ITEMS;item++) {
      const char* name="";bool numeric=true;
      switch(item) {
        case 0:name="< Back";value[0]=0;numeric=false;break;
        case 1:name="Speed";snprintf(value,sizeof(value),"%2u.%u",motionSettings.speedHalf/2,(motionSettings.speedHalf&1)*5);break;
        case 2:name="Curve";snprintf(value,sizeof(value),motionSettings.organic?"%3u%%":"N/A",motionSettings.curve);break;
        case 3:name="Coverage";snprintf(value,sizeof(value),motionSettings.organic?"%3u%%":"N/A",motionSettings.coverage);break;
        case 4:name="Info Cycle";snprintf(value,sizeof(value),"%2us",motionSettings.cycleSeconds);break;
        case 5:name="Motion";copyToBuffer(value,sizeof(value),motionSettings.organic?"Organic":"Bounce");numeric=false;break;
        case 6:name="Jerk Limit";if(!motionSettings.organic)copyToBuffer(value,sizeof(value),"N/A");else if(!motionSettings.jerk)copyToBuffer(value,sizeof(value),"Off");else snprintf(value,sizeof(value),"%3u%%",motionSettings.jerk);break;
        default:name="Defaults (hold)";value[0]=0;numeric=false;
      }
      drawSettingRow(item,state.motionIdx,offset,name,value,numeric);
    }
  }

  void drawInputSettings() {
    float offset=listOffset(state.inputOffset);char value[20];
    for(int item=floorf(offset);item<static_cast<int>(ceilf(offset))+5 && item<INPUT_ITEMS;item++) {
      const char* name="";
      switch(item) {
        case 0:name="< Back";value[0]=0;break;
        case 1:name="Debounce";snprintf(value,sizeof(value),"%2ums",inputSettings.debounceMs);break;
        case 2:name="Hold Delay";snprintf(value,sizeof(value),"%4ums",inputSettings.holdDelayMs);break;
        case 3:name="Repeat Wait";snprintf(value,sizeof(value),"%4ums",inputSettings.repeatDelayMs);break;
        case 4:name="Repeat Rate";snprintf(value,sizeof(value),"%2u/s",inputSettings.repeatHz);break;
        case 5:name="Chord";snprintf(value,sizeof(value),"%3ums",inputSettings.chordMs);break;
        default:name="Defaults (hold)";value[0]=0;
      }
      drawSettingRow(item,state.inputIdx,offset,name,value,item>0&&item<INPUT_ITEMS-1);
    }
  }

  void drawAlarmMission() {
    if(alarmMgr.lightMission()) {
      drawPanelHeader("Light Mission");u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.setCursor(10,31);u8g2.printf("Now    %3d%%",(4095-state.filteredCds)*100/4095);
      u8g2.setCursor(10,45);u8g2.printf("Target %3u%%",(4095-alarmMgr.lightTarget())*100/4095);
      u8g2.drawHLine(8,57,112);u8g2.drawBox(8,55,min(112UL,alarmMgr.lightProgress(millis())*112/3000),3);
    } else {
      drawPanelHeader(alarmMgr.showingKeys()?"Remember":"Repeat");u8g2.setFont(u8g2_font_ncenB12_tr);
      for(int i=0;i<4;i++){const char* mark=alarmMgr.showingKeys()?(alarmMgr.keyRight(i)?">":"<"):(i<alarmMgr.enteredKeys()?"+":"-");u8g2.drawStr(24+i*23,40,mark);}
    }
  }

  void drawCo2Setting() {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    int titleWidth=u8g2.getStrWidth("CO Alert Setup")+4;
    drawCo2Label((128-titleWidth)/2,11," Alert Setup");u8g2.drawHLine(0,14,128);
    u8g2.setFont(u8g2_font_6x10_tr);u8g2.drawStr(22,28,"Threshold");
    char number[8];snprintf(number,sizeof(number),"%4d",state.co2Limit);
    const char* value=state.co2Limit>3000?"Off":number;
    u8g2.setFont(u8g2_font_ncenB14_tr);int numberWidth=4*(u8g2.getStrWidth("00")-u8g2.getStrWidth("0"));
    u8g2.setFont(u8g2_font_ncenB08_tr);int unitWidth=u8g2.getStrWidth(" ppm");
    int x=(128-numberWidth-unitWidth)/2;
    u8g2.setFont(u8g2_font_ncenB14_tr);
    int valueX=state.co2Limit>3000?(128-u8g2.getStrWidth(value))/2:x;
    // Numerals are 14px high; Off is 15px. Both retain their entire top row.
    rolling.draw(u8g2,CO2_SETTING*100,value,valueX,state.co2Limit>3000?47:46,{x,32,numberWidth+unitWidth,15},state.valueDirection,millis(),true);
    if(state.co2Limit<=3000){u8g2.setFont(u8g2_font_ncenB08_tr);u8g2.drawStr(x+numberWidth,45," ppm");}
    // A decorative frame: this screen adjusts immediately and has no cursor.
    u8g2.drawFrame(x-4,30,numberWidth+unitWidth+8,19);
    u8g2.setFont(u8g2_font_6x10_tr);u8g2.drawStr(33,60,"OK to save");
  }

  void drawWiFiScanning() {
    if (apMgr.scanMessage == WIFI_SCAN_MESSAGE_DISCONNECTED) {
      drawLoading("Disconnected!\nFinding AP...", 40, true);
    } else if (apMgr.scanMessage == WIFI_SCAN_MESSAGE_CONNECTION_FAILED || apMgr.showFailMessage) {
      drawLoading("Connection Failed!\nFinding AP...", 40, true);
    } else {
      drawLoading("Finding AP...", 34, true);
    }
  }

  void drawWiFiList() {
    u8g2.setFont(u8g2_font_6x10_tr);
    int totalItems = apMgr.apList.size();
    if (totalItems == 0) {
      u8g2.setCursor(20, 35);
      u8g2.print("No APs found!");
      return;
    }

    int focusKey = uiPage(state.uiState) * 1000 + state.listIdx;

    float offset=listOffset(state.listOffset);
    for (int cIdx=floorf(offset); cIdx<static_cast<int>(ceilf(offset))+5; cIdx++) {
      if (cIdx >= totalItems) break;

      int y = 12 + oled::pixel((cIdx-offset)*12);
      APItem& item = apMgr.apList[cIdx];
      
      if (cIdx == state.listIdx) {
        queueFocus(0, clampValue(y-10,0,50), 128, 14); u8g2.setDrawColor(1);
      } else u8g2.setDrawColor(1);

      if (item.category < 0) {
        u8g2.setCursor(2, y);
        u8g2.print(item.ssid);
        continue;
      }

      int iconX = 115;
      u8g2.setFont(u8g2_font_waffle_t_all);
      u8g2.drawGlyph(iconX, y + 1, apMgr.getSignalIcon(item.rssi));
      iconX -= 12;

      if (item.isSecured) { u8g2.drawGlyph(iconX, y + 1, 58076); iconX -= 12; }
      
      if (state.uiState == WIFI_CONNECTING && item.ssid == apMgr.connectingSSID) {
        if (apMgr.connectStatus == 2) {
          u8g2.drawGlyph(iconX, y + 1, 58098);
        } else {
          // Show a spinner while a connection attempt is still in progress.
          drawSpinner(iconX + 5, y - 4);
        }
        iconX -= 12;
      } else if (item.category == 0) {
        u8g2.drawGlyph(iconX, y + 1, 58098); iconX -= 12;
      } else if (item.category == 1 || item.category == 3) {
        u8g2.drawGlyph(iconX, y + 1, 57671); iconX -= 12;
      }
      
      u8g2.setFont(u8g2_font_6x10_tr);
      int maxTextWidth = iconX - 2;
      if (cIdx == state.listIdx && item.hasDuplicate) {
        char displaySSID[128];
        snprintf(displaySSID, sizeof(displaySSID), "%s [%s]", item.ssid.c_str(), item.bssid.c_str());
        drawScrollableText(2, y, maxTextWidth, displaySSID, true, focusKey);
      } else {
        drawScrollableText(2, y, maxTextWidth, item.ssid.c_str(), cIdx == state.listIdx, focusKey);
      }
    }
    u8g2.setDrawColor(1);
  }

  void drawLocationResultList() {
    u8g2.setFont(u8g2_font_6x10_tr);

    int totalItems = static_cast<int>(locResults.size()) + 1;
    if (locResults.size() == 0) {
      u8g2.setCursor(10, 35);
      u8g2.print("No results found.");
    }

    int focusKey = uiPage(state.uiState) * 1000 + state.listIdx;

    float offset=listOffset(state.listOffset);
    for (int cIdx=floorf(offset); cIdx<static_cast<int>(ceilf(offset))+5; cIdx++) {
      if (cIdx >= totalItems) break;

      int y = 12 + oled::pixel((cIdx-offset)*12);
      if (cIdx == state.listIdx) {
        queueFocus(0, clampValue(y-9,0,51), 128, 13);
        u8g2.setDrawColor(1);
      } else {
        u8g2.setDrawColor(1);
      }

      u8g2.setCursor(4, y);
      if (cIdx == 0) {
        u8g2.print("< Back");
      } else {
        LocationResult& res = locResults[cIdx - 1];
        char displayStr[112];
        buildLocationResultLabel(res, displayStr, sizeof(displayStr));
        drawScrollableText(4, y, 122, displayStr, cIdx == state.listIdx, focusKey);
      }
      u8g2.setDrawColor(1);
    }
  }

} displayMgr;

// =================================================================================
// 4. Main Controller & Setup
// =================================================================================

// Full reset is staged so the spinner can keep animating while radio and saved data are cleared.
void performFullReset(unsigned long now) {
  state.fullResetPending = true;
  state.fullResetStage = 0;
  state.fullResetStageStarted = now;
  showLoadingOverlay("Resetting...\nPlease wait", true);
}

bool processPendingFullReset(unsigned long now) {
  if (!state.fullResetPending) return false;

  switch (state.fullResetStage) {
    case 0:
      if (now - state.fullResetStageStarted < 140) return true;
      // A completed result may be discarded by the imminent restart, but an
      // active HTTP operation must release the Wi-Fi stack before shutdown.
      if (appNetworkWorker.hasActiveWork()) return true;
      WiFi.scanDelete();
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      state.fullResetStage = 1;
      state.fullResetStageStarted = now;
      return true;

    case 1:
      if (now - state.fullResetStageStarted < 90) return true;
      {
        Preferences clearPrefs;
        clearPrefs.begin("clock", false);
        clearPrefs.clear();
        clearPrefs.end();
      }
      state.fullResetStage = 2;
      state.fullResetStageStarted = now;
      return true;

    case 2:
      if (now - state.fullResetStageStarted < 90) return true;
      {
        Preferences clearPrefs;
        clearPrefs.begin("wifi_db", false);
        clearPrefs.clear();
        clearPrefs.end();
      }
      state.fullResetStage = 3;
      state.fullResetStageStarted = now;
      return true;

    default:
      if (now - state.fullResetStageStarted < 180) return true;
      u8g2.clearDisplay();
      ESP.restart();
      return true;
  }
}

bool hasLocation() {
  return hasValidLatLon(state.lat, state.lon);
}

// Weather jobs are only enqueued when a usable location exists and the worker is free.
bool requestWeatherSync(NetworkJobContext context) {
  if (!hasLocation()) return false;
  if (WiFi.status() != WL_CONNECTED || appNetworkWorker.isBusy()) return false;

  bool accepted = appNetworkWorker.requestWeatherSync(
    context,
    state.lat,
    state.lon
  );
  if (accepted) state.lastWeatherAttemptTime = millis();
  return accepted;
}

bool requestLocationSearch(const String& query) {
  if (WiFi.status() != WL_CONNECTED || appNetworkWorker.isBusy()) return false;
  return appNetworkWorker.requestLocationSearch(query);
}
void handleWeatherRequestFailure(UIState fallbackState) {
  state.uiState = fallbackState;
  if (WiFi.status() != WL_CONNECTED) {
    showInfoOverlay("Connect WiFi first");
  } else if (appNetworkWorker.isBusy()) {
    showInfoOverlay("Network Busy");
  } else {
    showInfoOverlay("Weather Sync Failed");
  }
}
void handleLocationSearchRequestFailure() {
  if (WiFi.status() != WL_CONNECTED) {
    showInfoOverlay("Connect WiFi first");
  } else if (appNetworkWorker.isBusy()) {
    showInfoOverlay("Network Busy");
  } else {
    showInfoOverlay("Search Failed");
  }
}

void beginWiFiConnectionAttempt(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') {
    state.uiState = WIFI_LIST;
    showInfoOverlay("Select AP again");
    return;
  }

  char passBuf[65];
  copyToBuffer(passBuf, sizeof(passBuf), pass ? pass : "");

  ensureWiFiStationMode();
  apMgr.scanMessage = WIFI_SCAN_MESSAGE_NONE;
  apMgr.showFailMessage = false;
  copyToBuffer(apMgr.connectingSSID, sizeof(apMgr.connectingSSID), ssid);
  copyToBuffer(apMgr.connectingPass, sizeof(apMgr.connectingPass), passBuf);
  WiFi.scanDelete();
  if (apMgr.connectingPass[0] != '\0') WiFi.begin(apMgr.connectingSSID, apMgr.connectingPass);
  else WiFi.begin(apMgr.connectingSSID);
  apMgr.connectStartTime = millis();
  apMgr.connectStatus = 1;
  apMgr.moveAPToTop(apMgr.connectingSSID);
  state.listIdx = apMgr.findIndexBySSID(apMgr.connectingSSID);
  clampListState(static_cast<int>(apMgr.apList.size()));
  state.uiState = WIFI_CONNECTING;
}

void beginWiFiConnectionAttempt(const String& ssid, const char* pass) {
  beginWiFiConnectionAttempt(ssid.c_str(), pass);
}

void beginWiFiConnectionAttempt(const String& ssid, const String& pass) {
  beginWiFiConnectionAttempt(ssid.c_str(), pass.c_str());
}
void disconnectCurrentWiFiAndScan() {
  applyWiFiAutoReconnectPreference(false);
  WiFi.disconnect(false, false);
  state.uiState = WIFI_SCANNING;
  apMgr.startScan(WIFI_SCAN_MESSAGE_DISCONNECTED);
}

void applyWeatherResult(const NetworkResult& result, unsigned long now) {
  if (!result.success || !result.hasWeatherData) {
    state.isWeatherSynced = false;
    return;
  }

  state.tempOut = result.tempOut;
  state.weatherCode = result.weatherCode;
  state.isDay = result.isDay;
  state.rainProb = result.rainProb;
  state.pm10 = result.pm10;
  state.windSpeed = result.windSpeed;
  state.windDir = result.windDir;
  bool offsetChanged = rtc.offset != result.utcOffsetSeconds;
  rtc.offset = result.utcOffsetSeconds;
  alarmMgr.setCalendarOffset(result.utcOffsetSeconds);
  if (offsetChanged) alarmMgr.save();
  state.isWeatherSynced = true;
  state.lastSuccessfulWeatherUpdate = now;
  if (state.isTimeSynced) state.lastWeatherUpdateMinute = rtc.getMinute();
}

// Search results are copied into the UI-owned vector only on the main thread.
void applyLocationSearchResult(const NetworkResult& result) {
  locResults.clear();
  locResults.reserve(result.locationCount);
  for (int i = 0; i < result.locationCount; ++i) {
    LocationResult item;
    item.name = result.locationResults[i].name;
    item.country = result.locationResults[i].country;
    item.lat = result.locationResults[i].lat;
    item.lon = result.locationResults[i].lon;
    locResults.push_back(item);
  }
}

void finishStartupFlow() {
  startupFlow.phase = STARTUP_IDLE;
  startupFlow.phaseStartTime = 0;
  startupFlow.syncRetryCount = 0;
  startupFlow.waitingForWeather = false;
  startupFlow.foundSSID[0] = '\0';
  startupFlow.foundPass[0] = '\0';
  hideLoadingOverlay();
}

void beginStartupSync() {
  beginNtpTimeSync(millis());
  startupFlow.phase = STARTUP_WAIT_SYNC;
  startupFlow.phaseStartTime = millis();
  startupFlow.syncRetryCount = 0;
  startupFlow.waitingForWeather = false;
  showLoadingOverlay("WiFi Connected!\nUpdating Data...", true);
}

bool findBestSavedAP(int scannedCount, char* bestSsid, size_t bestSsidSize, char* bestPass, size_t bestPassSize) {
  int bestRssi = -100;
  if (bestSsidSize > 0) bestSsid[0] = '\0';
  if (bestPassSize > 0) bestPass[0] = '\0';

  for (int i = 0; i < scannedCount; ++i) {
    String ssid = WiFi.SSID(i);
    char passBuf[65];
    if (wifiStorage.getPassword(ssid.c_str(), passBuf) && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      copyToBuffer(bestSsid, bestSsidSize, ssid);
      copyToBuffer(bestPass, bestPassSize, passBuf);
    }
  }

  return bestSsidSize > 0 && bestSsid[0] != '\0';
}

void updateStartupFlow(unsigned long now) {
  switch (startupFlow.phase) {
    case STARTUP_IDLE:
      break;

    case STARTUP_NO_WIFI_INFO:
      showLoadingOverlay("No Saved WiFi\nPlease Setup Wifi", false);
      if (now - startupFlow.phaseStartTime >= STARTUP_FOUND_AP_INFO_MS) finishStartupFlow();
      break;

    case STARTUP_WAIT_INITIAL_CONNECT:
      {
        long remaining = (STARTUP_CONNECT_WAIT_MS - static_cast<long>(now - startupFlow.phaseStartTime) + 999) / 1000;
        if (remaining < 0) remaining = 0;
        char overlayText[48];
        snprintf(overlayText, sizeof(overlayText), "Connecting WiFi...\n%ld", remaining);
        showLoadingOverlay(overlayText, true);

        if (WiFi.status() == WL_CONNECTED) {
          beginStartupSync();
        } else if (now - startupFlow.phaseStartTime >= STARTUP_CONNECT_WAIT_MS) {
          WiFi.disconnect(true);
          startupFlow.phase = STARTUP_PREPARE_SCAN;
          startupFlow.phaseStartTime = now;
        }
      }
      break;

    case STARTUP_PREPARE_SCAN:
      showLoadingOverlay("WiFi Connect Fail\nFinding AP...", true);
      if (now - startupFlow.phaseStartTime >= STARTUP_SCAN_PREP_MS) {
        ensureWiFiStationMode();
        WiFi.scanDelete();
        WiFi.scanNetworks(true, false);
        startupFlow.phase = STARTUP_WAIT_SCAN;
        startupFlow.phaseStartTime = now;
      }
      break;

    case STARTUP_WAIT_SCAN:
      {
        showLoadingOverlay("WiFi Connect Fail\nFinding AP...", true);
        int n = WiFi.scanComplete();
        if (n >= 0) {
          findBestSavedAP(n, startupFlow.foundSSID, sizeof(startupFlow.foundSSID),
                          startupFlow.foundPass, sizeof(startupFlow.foundPass));
          WiFi.scanDelete();

          if (startupFlow.foundSSID[0] != '\0') {
            startupFlow.phase = STARTUP_FOUND_AP_INFO;
            startupFlow.phaseStartTime = now;
          } else {
            ensureWiFiStationMode();
            WiFi.begin();
            startupFlow.phase = STARTUP_WAIT_RECONNECT;
            startupFlow.phaseStartTime = now;
          }
        } else if (now - startupFlow.phaseStartTime >= STARTUP_SCAN_WAIT_MS) {
          WiFi.scanDelete();
          ensureWiFiStationMode();
          WiFi.begin();
          startupFlow.phase = STARTUP_WAIT_RECONNECT;
          startupFlow.phaseStartTime = now;
        }
      }
      break;

    case STARTUP_FOUND_AP_INFO:
      {
        char overlayText[96];
        snprintf(overlayText, sizeof(overlayText), "Found AP:\n%s", startupFlow.foundSSID);
        showLoadingOverlay(overlayText, false);
      }
      if (now - startupFlow.phaseStartTime >= STARTUP_FOUND_AP_INFO_MS) {
        ensureWiFiStationMode();
        WiFi.begin(startupFlow.foundSSID, startupFlow.foundPass);
        startupFlow.phase = STARTUP_WAIT_RECONNECT;
        startupFlow.phaseStartTime = now;
      }
      break;

    case STARTUP_WAIT_RECONNECT:
      {
        long remaining = (STARTUP_RECONNECT_WAIT_MS - static_cast<long>(now - startupFlow.phaseStartTime) + 999) / 1000;
        if (remaining < 0) remaining = 0;
        char overlayText[48];
        snprintf(overlayText, sizeof(overlayText), "Connecting WiFi...\n%ld", remaining);
        showLoadingOverlay(overlayText, true);

        if (WiFi.status() == WL_CONNECTED) {
          beginStartupSync();
        } else if (now - startupFlow.phaseStartTime >= STARTUP_RECONNECT_WAIT_MS) {
          finishStartupFlow();
        }
      }
      break;

    case STARTUP_WAIT_SYNC:
      {
        showLoadingOverlay("WiFi Connected!\nUpdating Data...", true);

        if (WiFi.status() != WL_CONNECTED) {
          startupFlow.phase = STARTUP_WAIT_RECONNECT;
          startupFlow.phaseStartTime = now;
          startupFlow.waitingForWeather = false;
          break;
        }

        if (now - startupFlow.phaseStartTime >= STARTUP_SYNC_TIMEOUT_MS) {
          // A timeout only closes the boot overlay. Initial time remains invalid
          // until the SNTP callback proves that a server response was received.
          finishStartupFlow();
          break;
        }

        if (!state.isTimeSynced || state.timeResyncPending) applyCompletedNtpTime();

        if (state.isTimeSynced && !state.isWeatherSynced &&
            !startupFlow.waitingForWeather &&
            startupFlow.syncRetryCount < MAX_SYNC_RETRY &&
            (state.lastWeatherAttemptTime == 0 || now - state.lastWeatherAttemptTime >= 1000)) {
          if (requestWeatherSync(NETWORK_CONTEXT_STARTUP)) {
            startupFlow.waitingForWeather = true;
          } else {
            startupFlow.syncRetryCount = MAX_SYNC_RETRY;
          }
        }

        if (state.isTimeSynced && state.isWeatherSynced) {
          finishStartupFlow();
        } else if (!startupFlow.waitingForWeather &&
                   startupFlow.syncRetryCount >= MAX_SYNC_RETRY) {
          finishStartupFlow();
        }
      }
      break;
  }
}

// Worker results are applied from the main loop so UI state changes stay serialized.
void handleNetworkResults(unsigned long now) {
  NetworkResult result;
  if (!appNetworkWorker.takeResult(result)) return;

  switch (result.type) {
    case NETWORK_JOB_WEATHER_SYNC: {
      if (result.context == NETWORK_CONTEXT_STARTUP) {
        startupFlow.waitingForWeather = false;
        if (!result.success) startupFlow.syncRetryCount++;
      }

      // Background results are snapshots. Ignore one if the user changed or
      // cleared the location while the HTTP request was running.
      bool weatherApplied = result.success && result.hasWeatherData &&
                            state.lat == result.lat && state.lon == result.lon;
      if (weatherApplied) {
        applyWeatherResult(result, now);
      }

      if (result.context == NETWORK_CONTEXT_LOCATION) {
        if (!weatherApplied) alarmMgr.setCalendarOffset(rtc.offset);
        hideLoadingOverlay();
        state.uiState = MAIN_MENU;
      }
      break;
    }

    case NETWORK_JOB_LOCATION_SEARCH:
      if (!result.success) {
        hideLoadingOverlay();
        state.uiState = LOCATION_SEARCH_INPUT;
        break;
      }
      applyLocationSearchResult(result);
      hideLoadingOverlay();
      state.listIdx = 0;
      state.listOffset = 0;
      state.uiState = LOCATION_RESULT_LIST;
      break;

    default:
      break;
  }
}

// Boot uses a small state machine instead of long blocking waits so rendering can continue during reconnect and sync.
void startStartupFlow() {
  WiFi.setAutoReconnect(state.wifiAutoReconnectEnabled);
  if (!state.wifiAutoReconnectEnabled) {
    WiFi.disconnect(true, false);
    startupFlow.phase = STARTUP_IDLE;
    startupFlow.phaseStartTime = 0;
    return;
  }

  ensureWiFiStationMode();
  if (wifiStorage.count > 0) {
    WiFi.begin();
    startupFlow.phase = STARTUP_WAIT_INITIAL_CONNECT;
    startupFlow.phaseStartTime = millis();
  } else {
    startupFlow.phase = STARTUP_NO_WIFI_INFO;
    startupFlow.phaseStartTime = millis();
  }
}

bool isStartupConnectionRecoverySkippable() {
  return startupFlow.phase == STARTUP_PREPARE_SCAN ||
         startupFlow.phase == STARTUP_WAIT_SCAN ||
         startupFlow.phase == STARTUP_FOUND_AP_INFO ||
         startupFlow.phase == STARTUP_WAIT_RECONNECT;
}

void skipStartupConnectionRecovery(unsigned long now) {
  WiFi.scanDelete();
  finishStartupFlow();
  state.uiState = SCREEN_SAVER;
  state.lastInputTime = now;
}

void handleMenuInactivity(unsigned long now) {
  if (state.uiState == SCREEN_SAVER || alarmOutputController.isTimeAlarmActive() ||
      loadingOverlay.active || state.fullResetPending) {
    return;
  }
  if (now - state.lastInputTime < MENU_INACTIVITY_TIMEOUT_MS) return;

  bool wasWiFiFlow = isWiFiFlowActive();
  if (wasWiFiFlow) {
    WiFi.scanDelete();
    apMgr.isScanning = false;
    apMgr.connectStatus = 0;
    apMgr.showFailMessage = false;
    apMgr.scanMessage = WIFI_SCAN_MESSAGE_NONE;
    apMgr.connectingSSID[0] = '\0';
    apMgr.connectingPass[0] = '\0';
    apMgr.releaseList();
  }

  if(state.uiState==MOTION_SETTING)saveMotionSettings();
  if(state.uiState==INPUT_SETTING)saveInputSettings();
  cancelMenuHold();
  state.lightLearning=false;
  releaseLocationResults();
  state.isEditing = false;
  state.editingAlarmIdx = -1;
  state.uiState = SCREEN_SAVER;
}

// Input handling is the single place where button events mutate UI state.
void processInput(unsigned long now) {
  bool u = btnLeft.isTriggered(), d = btnRight.isTriggered(), e = btnOk.isTriggered();
  bool leftPressed = btnLeft.down(), rightPressed = btnRight.down(), okPressed = btnOk.down();
  if(u||d)state.valueDirection=d?1:-1;
  bool userNavigation=e;
  struct NavigationFeedback {
    UIState before;bool& activated;
    ~NavigationFeedback(){if(activated && uiPage(before)!=uiPage(state.uiState))displayMgr.navigationActivated(before);}
  } navigationFeedback{state.uiState,userNavigation};
  if(uiHold.running() && uiHold.key>=0 && uiHold.key/1000!=state.uiState)cancelMenuHold();
  bool bothHeld = leftPressed && rightPressed;
  bool anyButtonHeld = leftPressed || rightPressed || okPressed;
  static bool navigationChordActive = false;
  static bool screenWakeGestureActive = false;
  static bool startupSkipGestureActive = false;
  static unsigned long alarmListFirstPressTime = 0;
  static uint8_t alarmListPendingBtn = 0;
  static bool alarmListActionExecuted = false;
  static unsigned long observedAlarmStartTime = 0;
  static bool alarmDismissRequiresRelease = false;

  bool timeAlarmActive = alarmOutputController.isTimeAlarmActive();
  if(state.uiState!=WIFI_LIST || timeAlarmActive || alarmMgr.missionActive()) {
    state.wifiHoldingOk=false;state.wifiLongHandled=false;
  }
  if (timeAlarmActive && observedAlarmStartTime != state.alarmStartTime) {
    observedAlarmStartTime = state.alarmStartTime;
    alarmDismissRequiresRelease = okPressed || bothHeld;
  } else if (!timeAlarmActive) {
    observedAlarmStartTime = 0;
    alarmDismissRequiresRelease = false;
  }

  if (state.uiState != ALARM_LIST) {
    alarmListPendingBtn = 0;
    alarmListActionExecuted = false;
  }

  // A press that began while the OLED was off belongs exclusively to wake-up.
  // Consume the complete press/release gesture so a held or repeating button
  // cannot leak into alarm dismissal, CO2 mute, navigation, or editing.
  if (screenWakeGestureActive) {
    alarmListPendingBtn = 0;
    alarmListActionExecuted = false;
    state.lastInputTime = now;
    if (!anyButtonHeld) screenWakeGestureActive = false;
    return;
  }

  if (state.isScreenOff && anyButtonHeld) {
    cancelMenuHold();
    alarmListPendingBtn = 0;
    alarmListActionExecuted = false;
    state.lastInputTime = now;
    state.isScreenOff = false;
    u8g2.setPowerSave(0);
    screenWakeGestureActive = true;
    return;
  }

  // Once a startup-skip chord begins, consume it through the complete release
  // so it cannot also dismiss a newly starting alarm or mute CO2 afterward.
  if (startupSkipGestureActive) {
    state.lastInputTime = now;
    if (isStartupConnectionRecoverySkippable() && uiHold.poll(bothHeld,now)) {
      skipStartupConnectionRecovery(now);
    }
    if (!anyButtonHeld) {
      startupSkipGestureActive = false;
      uiHold.cancel();
    }
    return;
  }

  if (u || d || e || bothHeld) {
    state.lastInputTime = now;
  }

  if (timeAlarmActive) {
    cancelMenuHold();
    alarmListPendingBtn = 0;
    alarmListActionExecuted = false;
    if (alarmDismissRequiresRelease) {
      if (!okPressed && !bothHeld) alarmDismissRequiresRelease = false;
      return;
    }
    bool canDismiss = (now - state.alarmStartTime) >= ALARM_DISMISS_GUARD_MS;
    if (canDismiss && (e || bothHeld)) {
      if (bothHeld) navigationChordActive = true;
      alarmMgr.acknowledge(now,bothHeld);
    }
    return;
  }

  if(alarmMgr.missionActive()) {
    cancelMenuHold();
    alarmMgr.missionInput(u,d,anyButtonHeld,now);
    return;
  }

  if (isStartupConnectionRecoverySkippable() && bothHeld) {
    startupSkipGestureActive = true;
    uiHold.begin(-2,now,inputSettings.holdDelayMs);
    return;
  }

  if (loadingOverlay.active && loadingOverlay.blocksInput) {cancelMenuHold();return;}

  bool textInputActive = state.uiState == WIFI_PASS_INPUT ||
                         state.uiState == LOCATION_SEARCH_INPUT;
  if (!textInputActive && state.uiState != ALARM_LIST && bothHeld) {
    cancelMenuHold();
    if (!navigationChordActive) {
      muteCo2AlertIfRequested();
    }
    navigationChordActive = true;
    return;
  }
  if (navigationChordActive) {
    // Consume staggered button releases so they cannot leak back as left/right events.
    if (!leftPressed && !rightPressed) navigationChordActive = false;
    return;
  }

  switch (state.uiState) {
    case SCREEN_SAVER:
      if (u) stepWeatherInfoCycle(-1, now);
      if (d) stepWeatherInfoCycle(1, now);
      if (e) state.uiState = MAIN_MENU;
      break;

    case MAIN_MENU:
      if (u && state.menuIdx > 0) {
        state.menuIdx--;
        if (state.menuIdx < state.menuOffset) state.menuOffset--;
      }
      if (d && state.menuIdx < MAIN_ITEMS-1) {
        state.menuIdx++;
        if (state.menuIdx >= state.menuOffset + 4) state.menuOffset++;
      }
      if (e) {
        int m = state.menuIdx;
        if (m == 0) {
          state.uiState = SCREEN_SAVER;
        }
        else if (m == 1) { state.uiState = ALARM_LIST; state.listIdx = 0; state.listOffset = 0; }
        else if (m == 2) state.uiState = CO2_SETTING;
        else if (m == 3) {
          state.abcConfirmTargetOn = !state.abcEnabled;
          state.abcConfirmYes = false;
          state.uiState = ABC_CONFIRM;
        }
        else if (m == 4) { state.calConfirmYes = false; state.uiState = CALIBRATION_CONFIRM; }
        else if (m == 5) state.uiState = LOCATION_SETUP;
        else if (m == 6) {
          if (appNetworkWorker.isBusy()) {
            showInfoOverlay("Network Busy");
          } else if (WiFi.getMode() == WIFI_OFF) {
            apMgr.isScanning = false;
            apMgr.showFailMessage = false;
            apMgr.scanMessage = WIFI_SCAN_MESSAGE_NONE;
            apMgr.buildAndSortList(0);
            state.uiState = WIFI_LIST;
            state.listIdx = 0;
            state.listOffset = 0;
          } else {
            state.uiState = WIFI_SCANNING;
            apMgr.startScan();
          }
        }
        else if (m == MAIN_ITEMS-1) {
          cancelMenuHold();
          state.resetConfirmStep = 0;
          state.resetConfirmYes = false;
          state.uiState = RESET_CONFIRM;
        }
        else if(m==7){state.motionIdx=0;state.motionOffset=0;state.isEditing=false;state.uiState=MOTION_SETTING;}
        else if(m==8){state.inputIdx=0;state.inputOffset=0;state.isEditing=false;state.uiState=INPUT_SETTING;}
      }
      break;

    case ALARM_LIST:
      {
        int total = alarmMgr.alarmCount + 2;
        clampListState(total);
        uint8_t navigation = 0;

        if (bothHeld) {
          if (!navigationChordActive) {
            if (state.listIdx > 0 && state.listIdx <= alarmMgr.alarmCount && !alarmMgr.sessionActive()) {
              Alarm& alarm = alarmMgr.alarms[state.listIdx - 1];
              alarm.enabled = !alarm.enabled;
              alarmMgr.save();
            } else {
              muteCo2AlertIfRequested();
            }
          }
          alarmListPendingBtn = 0;
          alarmListActionExecuted = false;
          navigationChordActive = true;
          break;
        }

        if (leftPressed || rightPressed) {
          uint8_t activeBtn = leftPressed ? 1 : 2;
          if (alarmListPendingBtn == 0 || alarmListPendingBtn != activeBtn) {
            alarmListPendingBtn = activeBtn;
            alarmListFirstPressTime = now;
            alarmListActionExecuted = false;
          } else if (!alarmListActionExecuted &&
                     now - alarmListFirstPressTime > DUAL_BUTTON_GRACE_MS) {
            navigation = alarmListPendingBtn;
            alarmListActionExecuted = true;
          } else if (alarmListActionExecuted &&
                     ((alarmListPendingBtn == 1 && u) || (alarmListPendingBtn == 2 && d))) {
            navigation = alarmListPendingBtn;
          }
        } else {
          if (alarmListPendingBtn != 0 && !alarmListActionExecuted) {
            navigation = alarmListPendingBtn;
          }
          alarmListPendingBtn = 0;
          alarmListActionExecuted = false;
        }

        if (navigation == 1 && state.listIdx > 0) {
          state.listIdx--;
          if (state.listIdx < state.listOffset) state.listOffset--;
        }
        if (navigation == 2 && state.listIdx < total - 1) {
          state.listIdx++;
          if (state.listIdx >= state.listOffset + 5) state.listOffset++;
        }
        if (e) {
          if (state.listIdx == 0) state.uiState = MAIN_MENU;
          else if(alarmMgr.sessionActive())showInfoOverlay("Alarm pending");
          else if (state.listIdx == total - 1) {
            if (alarmMgr.alarmCount < MAX_ALARMS) {
              state.editingAlarmIdx = -1;
              state.tempAlarm = { 0, 0, 0, true };
              state.tempAlarmOptions = AlarmOptions{};
              state.editRow = 0;
              state.isEditing = false;
              state.uiState = ALARM_EDIT;
            }
          } else {
            state.editingAlarmIdx = state.listIdx - 1;
            state.tempAlarm = alarmMgr.alarms[state.editingAlarmIdx];
            state.tempAlarmOptions = alarmMgr.options[state.editingAlarmIdx];
            state.editRow = 0;
            state.isEditing = false;
            state.uiState = ALARM_EDIT;
          }
        }
        break;
      }

    case ALARM_EDIT:
      if (!state.isEditing) {
        if (u) state.editRow = (state.editRow - 1 + 8) % 8;
        if (d) state.editRow = (state.editRow + 1) % 8;

        if (e) {
          if (state.editRow == 0) state.tempAlarm.enabled = !state.tempAlarm.enabled;
          else if (state.editRow == 3) { state.daySelectIdx = 0; state.uiState = ALARM_DAY_SELECT; }
          else if (state.editRow == 4) {state.optionIdx=0;state.optionOffset=0;state.uiState=ALARM_OPTIONS;}
          else if (state.editRow == 5) state.uiState = ALARM_LIST;
          else if (state.editRow == 6) {
            if(alarmMgr.sessionActive()) {showInfoOverlay("Alarm pending");break;}
            if((state.tempAlarmOptions.mission&2) && state.tempAlarmOptions.lightRaw>4095) {showInfoOverlay("Learn light first");break;}
            int index=state.editingAlarmIdx;
            if(index<0){if(alarmMgr.alarmCount>=MAX_ALARMS)break;index=alarmMgr.alarmCount++;}
            alarmMgr.alarms[index]=state.tempAlarm;alarmMgr.options[index]=state.tempAlarmOptions;
            alarmMgr.save();
            state.uiState = ALARM_LIST;
          } else if (state.editRow == 7) {
            if(alarmMgr.sessionActive()) {showInfoOverlay("Alarm pending");break;}
            if (state.editingAlarmIdx != -1) {
              for (int i = state.editingAlarmIdx; i < alarmMgr.alarmCount - 1; i++) {
                alarmMgr.alarms[i] = alarmMgr.alarms[i + 1];
                alarmMgr.options[i] = alarmMgr.options[i + 1];
              }
              alarmMgr.alarmCount--;
              alarmMgr.save();
            }
            state.uiState = ALARM_LIST;
          } else {
            state.isEditing = true;
          }
        }
      } else {
        if (e) state.isEditing = false;
        else if (state.editRow == 1) {
          if (u) state.tempAlarm.hour = (state.tempAlarm.hour + 23) % 24;
          if (d) state.tempAlarm.hour = (state.tempAlarm.hour + 1) % 24;
        } else if (state.editRow == 2) {
          if (u) state.tempAlarm.minute = (state.tempAlarm.minute + 59) % 60;
          if (d) state.tempAlarm.minute = (state.tempAlarm.minute + 1) % 60;
        }
      }
      break;

    case ALARM_DAY_SELECT:
      if (u) state.daySelectIdx = (state.daySelectIdx - 1 + 8) % 8;
      if (d) state.daySelectIdx = (state.daySelectIdx + 1) % 8;
      if (e) {
        if (state.daySelectIdx == 7) state.uiState = ALARM_EDIT;
        else state.tempAlarm.days ^= (1 << state.daySelectIdx);
      }
      break;

    case CO2_SETTING:
      if (u && state.co2Limit > 500) state.co2Limit -= 100;
      if (d && state.co2Limit < 3100) state.co2Limit += 100;
      if (e) { alarmMgr.save(); state.uiState = MAIN_MENU; }
      break;

    case CALIBRATION_CONFIRM:
      if (u || d) state.calConfirmYes = !state.calConfirmYes;
      if (e) {
        if (state.calConfirmYes) {
          sensorMgr.calibrateBaseline();
        }
        state.uiState = MAIN_MENU;
      }
      break;

    case ABC_CONFIRM:
      if (u || d) state.abcConfirmYes = !state.abcConfirmYes;
      if (e) {
        if (state.abcConfirmYes) {
          state.abcEnabled = state.abcConfirmTargetOn;
          sensorMgr.setAutoCalibration(state.abcEnabled);
          alarmMgr.save();
        }
        state.uiState = MAIN_MENU;
      }
      break;

    case RESET_CONFIRM:
      if (u || d) {
        cancelMenuHold();
        state.resetConfirmYes = !state.resetConfirmYes;
        break;
      }
      if (!state.resetConfirmYes) {
        cancelMenuHold();
        if (e) state.uiState = MAIN_MENU;
        break;
      }
      if(e)uiHold.begin(RESET_CONFIRM*1000+state.resetConfirmStep,now,FULL_RESET_HOLD_MS);
      if(uiHold.poll(okPressed,now)) {
        if(state.resetConfirmStep==0) {
          state.resetConfirmStep=1;
          state.resetConfirmYes=false;
        } else performFullReset(now);
      }
      break;

    case LOCATION_SETUP:
      {
        const int menuCount = 3;
        if (u) state.locMenuIdx = (state.locMenuIdx - 1 + menuCount) % menuCount;
        if (d) state.locMenuIdx = (state.locMenuIdx + 1) % menuCount;
      }
      if (e) {
        if (state.locMenuIdx == 0) state.uiState = MAIN_MENU;
        else if (state.locMenuIdx == 1) {
          releaseLocationResults();
          textInputMgr.reset("", 20);
          state.uiState = LOCATION_SEARCH_INPUT;
        } else if (state.locMenuIdx == 2) {
          rtc.offset = 0;
          state.cityName[0] = '\0';
          state.lat = "";
          state.lon = "";
          alarmMgr.setCalendarOffset(0);
          state.isWeatherSynced = false;
          state.lastWeatherAttemptTime = millis();
          alarmMgr.save();
        }
      }
      break;

    case MOTION_SETTING:
      if(state.motionIdx==MOTION_ITEMS-1) {
        if(e)beginMenuHold(state.motionIdx,now);
        if(uiHold.poll(okPressed,now)) {motionSettings=oled::MotionSettings{};saveMotionSettings();showInfoOverlay("Motion restored");}
        if(okPressed)break;
      }
      if(e) {
        if(state.motionIdx==0){saveMotionSettings();state.uiState=MAIN_MENU;}
        else if(state.motionIdx==MOTION_ITEMS-1)break;
        else if(!motionSettings.organic && (state.motionIdx==2||state.motionIdx==3||state.motionIdx==6))showInfoOverlay("Organic only");
        else {state.isEditing=!state.isEditing;if(!state.isEditing)saveMotionSettings();}
      } else if(state.isEditing) {
        int step=d?1:u?-1:0;
        switch(state.motionIdx) {
          case 1:motionSettings.speedHalf=clampValue(static_cast<int>(motionSettings.speedHalf)+step,2,36);break;
          case 2:motionSettings.curve=clampValue(static_cast<int>(motionSettings.curve)+step*5,0,100);break;
          case 3:motionSettings.coverage=clampValue(static_cast<int>(motionSettings.coverage)+step*5,0,100);break;
          case 4:motionSettings.cycleSeconds=clampValue(static_cast<int>(motionSettings.cycleSeconds)+step,2,10);if(step)state.lastWeatherInfoCycleChange=now;break;
          case 5:if(step)motionSettings.organic=!motionSettings.organic;break;
          case 6:motionSettings.jerk=clampValue(static_cast<int>(motionSettings.jerk)+step*5,0,100);break;
        }
      } else {
        if(u&&state.motionIdx>0)--state.motionIdx;if(d&&state.motionIdx<MOTION_ITEMS-1)++state.motionIdx;
        if(state.motionIdx<state.motionOffset)state.motionOffset=state.motionIdx;
        if(state.motionIdx>=state.motionOffset+5)state.motionOffset=state.motionIdx-4;
      }
      break;

    case INPUT_SETTING:
      if(state.inputIdx==INPUT_ITEMS-1) {
        if(e)beginMenuHold(state.inputIdx,now);
        if(uiHold.poll(okPressed,now)) {inputSettings=oled::InputSettings{};saveInputSettings();showInfoOverlay("Input restored");}
        if(okPressed)break;
      }
      if(e) {
        if(state.inputIdx==0){saveInputSettings();state.uiState=MAIN_MENU;}
        else if(state.inputIdx<INPUT_ITEMS-1){state.isEditing=!state.isEditing;if(!state.isEditing)saveInputSettings();}
      } else if(state.isEditing) {
        int step=d?1:u?-1:0;
        switch(state.inputIdx) {
          case 1:inputSettings.debounceMs=clampValue(int(inputSettings.debounceMs)+step*5,5,50);break;
          case 2:inputSettings.holdDelayMs=clampValue(int(inputSettings.holdDelayMs)+step*50,int(oled::InputSettings::MIN_HOLD_MS),int(oled::InputSettings::MAX_HOLD_MS));break;
          case 3:inputSettings.repeatDelayMs=clampValue(int(inputSettings.repeatDelayMs)+step*50,int(oled::InputSettings::MIN_REPEAT_MS),int(oled::InputSettings::MAX_REPEAT_MS));break;
          case 4:inputSettings.repeatHz=clampValue(int(inputSettings.repeatHz)+step,2,10);break;
          case 5:inputSettings.chordMs=clampValue(int(inputSettings.chordMs)+step*10,30,120);break;
        }
      } else {
        if(u&&state.inputIdx>0)--state.inputIdx;if(d&&state.inputIdx<INPUT_ITEMS-1)++state.inputIdx;
        if(state.inputIdx<state.inputOffset)state.inputOffset=state.inputIdx;
        if(state.inputIdx>=state.inputOffset+5)state.inputOffset=state.inputIdx-4;
      }
      break;

    case ALARM_OPTIONS:
      if(e) {
        if(state.optionIdx==0){state.uiState=ALARM_EDIT;state.editRow=4;}
        else if(state.optionIdx==1){state.snoozeIdx=0;state.uiState=ALARM_SNOOZE;}
        else if(state.optionIdx==5){state.lightIdx=0;state.lightLearning=false;state.uiState=ALARM_LIGHT;}
        else state.isEditing=!state.isEditing;
      } else if(state.isEditing) {
        int step=d?1:u?-1:0;
        if(state.optionIdx==2)state.tempAlarmOptions.checkMinutes=clampValue(static_cast<int>(state.tempAlarmOptions.checkMinutes)+step,0,10);
        else if(state.optionIdx==3)state.tempAlarmOptions.checkTimeoutSec=clampValue(static_cast<int>(state.tempAlarmOptions.checkTimeoutSec)+step*10,10,180);
        else if(state.optionIdx==4 && step)state.tempAlarmOptions.mission=(state.tempAlarmOptions.mission+step+4)%4;
      } else {
        if(u&&state.optionIdx>0)--state.optionIdx;if(d&&state.optionIdx<ALARM_OPTION_ITEMS-1)++state.optionIdx;
        if(state.optionIdx<state.optionOffset)state.optionOffset=state.optionIdx;
        if(state.optionIdx>=state.optionOffset+5)state.optionOffset=state.optionIdx-4;
      }
      break;

    case ALARM_SNOOZE:
      if(e) {if(state.snoozeIdx==0){state.uiState=ALARM_OPTIONS;state.optionIdx=1;state.optionOffset=0;}else state.isEditing=!state.isEditing;}
      else if(state.isEditing) {
        int step=d?1:u?-1:0;
        if(state.snoozeIdx==1)state.tempAlarmOptions.snoozeMinutes=clampValue(static_cast<int>(state.tempAlarmOptions.snoozeMinutes)+step,1,20);
        else state.tempAlarmOptions.snoozeCount=clampValue(static_cast<int>(state.tempAlarmOptions.snoozeCount)+step,0,5);
      } else {if(u)state.snoozeIdx=(state.snoozeIdx+2)%3;if(d)state.snoozeIdx=(state.snoozeIdx+1)%3;}
      break;

    case ALARM_LIGHT:
      if(state.lightLearning)break;
      if(u||d)state.lightIdx=1-state.lightIdx;
      if(e){if(state.lightIdx==0){state.uiState=ALARM_OPTIONS;state.optionIdx=5;state.optionOffset=1;}
        else{state.lightLearning=true;state.lightLearnStart=now;state.lightLearnSample=now;state.lightSampleCount=0;}}
      break;

    case LOCATION_SEARCH_INPUT:
      {
        int res = textInputMgr.processInput(e);
        if (res == 1) {
          String query = textInputMgr.getInputString();
          if (query.length() == 0) {
            break;
          }
          if (requestLocationSearch(query)) {
            showLoadingOverlay("Searching...", true);
          } else {
            handleLocationSearchRequestFailure();
          }
        } else if (res == 2) {
          state.uiState = LOCATION_SETUP;
        }
      }
      break;

    case LOCATION_RESULT_LIST:
      {
        int total = locResults.size() + 1;
        clampListState(total);
        if (u && state.listIdx > 0) {
          state.listIdx--;
          if (state.listIdx < state.listOffset) state.listOffset--;
        }
        if (d && state.listIdx < total - 1) {
          state.listIdx++;
          if (state.listIdx >= state.listOffset + 5) state.listOffset++;
        }

        if (e) {
          if (state.listIdx == 0) {
            releaseLocationResults();
            state.uiState = LOCATION_SEARCH_INPUT;
          } else {
            LocationResult& selected = locResults[state.listIdx - 1];
            copyToBuffer(state.cityName, sizeof(state.cityName), selected.name);
            // Do not show the previous location's time while resolving the new one.
            rtc.offset = 0;
            state.lat = selected.lat;
            state.lon = selected.lon;
            state.isWeatherSynced = false;
            state.lastWeatherAttemptTime = 0;
            alarmMgr.save();
            releaseLocationResults();
            if (requestWeatherSync(NETWORK_CONTEXT_LOCATION)) {
              showLoadingOverlay("Updating Weather...", true);
            } else {
              alarmMgr.setCalendarOffset(0);
              handleWeatherRequestFailure(MAIN_MENU);
            }
          }
        }
      }
      break;

    case WIFI_SCANNING:
      if (apMgr.isScanning) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          apMgr.isScanning = false;
          apMgr.buildAndSortList(n);
          WiFi.scanDelete();
          state.uiState = WIFI_LIST;
          if (state.wifiListReturnCategory != 0) {
            state.listIdx = apMgr.findIndexByCategory(state.wifiListReturnCategory);
            state.wifiListReturnCategory = 0;
            clampListState(static_cast<int>(apMgr.apList.size()));
          } else {
            state.listIdx = 0;
            state.listOffset = 0;
          }
        } else if (WiFi.getMode() == WIFI_OFF ||
                   millis() - apMgr.scanStartTime > WIFI_SCAN_TIMEOUT_MS) {
          apMgr.isScanning = false;
          WiFi.scanDelete();
          apMgr.buildAndSortList(0);
          state.uiState = WIFI_LIST;
          if (state.wifiListReturnCategory != 0) {
            state.listIdx = apMgr.findIndexByCategory(state.wifiListReturnCategory);
            state.wifiListReturnCategory = 0;
          } else {
            state.listIdx = 0;
            state.listOffset = 0;
          }
        }
      }
      break;

    case WIFI_LIST:
      {
        int totalItems = static_cast<int>(apMgr.apList.size());
        if (totalItems <= 0) {
          state.listIdx = 0;
          state.listOffset = 0;
          break;
        }

        clampListState(totalItems);
        if (apMgr.connectStatus > 0) return;
        bool& isHoldingOk=state.wifiHoldingOk;
        bool& longPressHandled=state.wifiLongHandled;

        if (!isHoldingOk) {
          if (u && state.listIdx > 0) {
            state.listIdx--;
            if (state.listIdx < state.listOffset) state.listOffset--;
          }
          if (d && state.listIdx < totalItems - 1) {
            state.listIdx++;
            if (state.listIdx >= state.listOffset + 5) state.listOffset++;
          }
        }

        if (e && !isHoldingOk) {
          isHoldingOk = true;
          longPressHandled = false;
          APItem& selected=apMgr.apList[state.listIdx];
          if(selected.category==1 || selected.category==3)beginMenuHold(state.listIdx,now);
        }

        if (isHoldingOk) {
          if (okPressed) {
            if (!longPressHandled && uiHold.poll(true,now)) {
              longPressHandled = true;
              APItem& selected = apMgr.apList[state.listIdx];
              if (selected.category == 1 || selected.category == 3) {
                isHoldingOk = false;
                wifiStorage.remove(selected.ssid.c_str());
                apMgr.startScan();
                state.uiState = WIFI_SCANNING;
              }
            }
          } else {
            isHoldingOk = false;
            uiHold.cancel();
            if (!longPressHandled) {
              userNavigation=true;
              APItem& selected = apMgr.apList[state.listIdx];
              if (selected.category < 0) {
                if (selected.category == -3) {
                  state.uiState = MAIN_MENU;
                  apMgr.releaseList();
                } else if (selected.category == -2) {
                  if (WiFi.getMode() == WIFI_OFF) {
                    state.wifiListReturnCategory = -2;
                    state.uiState = WIFI_SCANNING;
                    apMgr.startScan();
                  } else {
                    applyWiFiAutoReconnectPreference(false);
                    apMgr.isScanning = false;
                    apMgr.connectStatus = 0;
                    apMgr.showFailMessage = false;
                    apMgr.scanMessage = WIFI_SCAN_MESSAGE_NONE;
                    WiFi.scanDelete();
                    WiFi.disconnect(true, false);
                    apMgr.buildAndSortList(0);
                    state.listIdx = apMgr.findIndexByCategory(-2);
                    state.listOffset = 0;
                    state.uiState = WIFI_LIST;
                  }
                } else if (selected.category == -1) {
                  apMgr.showFailMessage = false;
                  state.uiState = WIFI_SCANNING;
                  apMgr.startScan();
                } else {
                  state.uiState = MAIN_MENU;
                  apMgr.releaseList();
                }
              }
              else if (selected.category == 0) {
                disconnectCurrentWiFiAndScan();
              }
              else {
                char savedPass[65];
                if (wifiStorage.getPassword(selected.ssid.c_str(), savedPass)) {
                  beginWiFiConnectionAttempt(selected.ssid, savedPass);
                } else {
                  if (!selected.isSecured) {
                    beginWiFiConnectionAttempt(selected.ssid, "");
                  } else {
                    copyToBuffer(apMgr.connectingSSID, sizeof(apMgr.connectingSSID), selected.ssid);
                    textInputMgr.reset("", 64);
                    state.uiState = WIFI_PASS_INPUT;
                  }
                }
              }
            }
            longPressHandled = false;
          }
        }
      }
      break;

    case WIFI_PASS_INPUT:
      {
        int res = textInputMgr.processInput(e);
        if (res == 1) {
          char passBuf[65];
          copyToBuffer(passBuf, sizeof(passBuf), textInputMgr.getInputString());
          beginWiFiConnectionAttempt(apMgr.connectingSSID, passBuf);
        } else if (res == 2) {
          state.uiState = WIFI_LIST;
        }
      }
      break;

    case WIFI_CONNECTING:
      if (apMgr.connectStatus == 1) {
        if (WiFi.status() == WL_CONNECTED) {
          applyWiFiAutoReconnectPreference(true);
          wifiStorage.addOrUpdate(apMgr.connectingSSID, apMgr.connectingPass);
          for (auto& ap : apMgr.apList) {
            if (ap.ssid == apMgr.connectingSSID) ap.category = 0;
            else if (ap.category == 0) ap.category = 1;
          }
          apMgr.refreshSystemItems();
          apMgr.sortList();
          state.listIdx = apMgr.findIndexBySSID(apMgr.connectingSSID);
          clampListState(static_cast<int>(apMgr.apList.size()));
          state.isWeatherSynced = false;
          state.lastWeatherAttemptTime = 0;
          beginNtpTimeSync(millis());
          apMgr.connectStatus = 0;
          apMgr.scanMessage = WIFI_SCAN_MESSAGE_NONE;
          state.uiState = WIFI_LIST;
        } else if (millis() - apMgr.connectStartTime > 10000) {
          WiFi.disconnect(false, false);
          apMgr.connectStatus = 0;
          apMgr.showFailMessage = true;
          apMgr.scanMessage = WIFI_SCAN_MESSAGE_CONNECTION_FAILED;

          bool isSecured = false;
          for (auto& ap : apMgr.apList) {
            if (ap.ssid == apMgr.connectingSSID) {
              isSecured = ap.isSecured;
              break;
            }
          }

          if (isSecured) {
            textInputMgr.reset(apMgr.connectingPass, 64);
            state.uiState = WIFI_PASS_INPUT;
          } else {
            state.uiState = WIFI_SCANNING;
            apMgr.startScan(WIFI_SCAN_MESSAGE_CONNECTION_FAILED);
          }
        }
      }
      break;
  }
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  alarmOutputController.init();

  displayMgr.init();
  alarmMgr.load();
  wifiStorage.load();
  sensorMgr.init();

  sntp_set_time_sync_notification_cb(onNtpTimeSynchronized);
  appNetworkWorker.init();
  startStartupFlow();
  state.lastInputTime = millis();
  state.lastWeatherInfoCycleChange = state.lastInputTime;
}
// Values read again during this iteration use this loop timestamp. A local
// millis() is reserved for subsystem timers first checked on a later iteration.
void loop() {
  unsigned long now = millis();
  if (processPendingFullReset(now)) {
    if (!state.isScreenOff) displayMgr.updateAndDraw();
    return;
  }
  if (loadingOverlay.active && !loadingOverlay.blocksInput &&
      loadingOverlay.expiresAt != 0 &&
      static_cast<int32_t>(now - loadingOverlay.expiresAt) >= 0) {
    hideLoadingOverlay();
  }
  updateAmbientLightState();
  alarmMgr.service(now);
  if(state.lightLearning && state.uiState==ALARM_LIGHT) {
    if(now-state.lightLearnSample>=50 && state.lightSampleCount<40) {
      state.lightLearnSample=now;state.lightSamples[state.lightSampleCount++]=state.filteredCds;
    }
    if(now-state.lightLearnStart>=1800 && state.lightSampleCount) {
      std::sort(state.lightSamples,state.lightSamples+state.lightSampleCount);
      state.tempAlarmOptions.lightRaw=state.lightSamples[state.lightSampleCount/2];
      state.lightLearning=false;
    }
  }
  updateWeatherInfoCycle(now);
  sensorMgr.applyLatestReading();
  handleNetworkResults(now);
  updateStartupFlow(now);
  processInput(now);
  handleMenuInactivity(now);

  if (processPendingFullReset(now)) {
    if (!state.isScreenOff) displayMgr.updateAndDraw();
    return;
  }

  static unsigned long lastSec = 0;
  if (now - lastSec > 1000) {
    lastSec = now;
    alarmMgr.check();
  }

  static unsigned long last2Sec = 0;
  if (now - last2Sec > SENSOR_READ_PERIOD) {
    last2Sec = now;
    if (startupFlow.phase == STARTUP_IDLE && WiFi.status() == WL_CONNECTED) {
      if (!state.isTimeSynced && !state.timeResyncPending) {
        beginNtpTimeSync(now);
      } else if ((!state.isTimeSynced || state.timeResyncPending) &&
          (state.lastTimeSyncAttemptTime == 0 || now - state.lastTimeSyncAttemptTime >= TIME_RETRY_MS)) {
        state.lastTimeSyncAttemptTime = now;
        applyCompletedNtpTime();
      } else if (!state.isWeatherSynced &&
                 canRunBackgroundNetworkTask() &&
                 hasLocation()) {
        if ((state.lastWeatherAttemptTime == 0 || now - state.lastWeatherAttemptTime > WEATHER_RETRY_MS) &&
            !appNetworkWorker.isBusy()) {
          requestWeatherSync(NETWORK_CONTEXT_BACKGROUND);
        }
      }
    }
  }

  static unsigned long lastMin = 0;
  if (now - lastMin > 60000) {
    lastMin = now;
    bool shouldReconnect = state.wifiAutoReconnectEnabled &&
                           wifiStorage.count > 0 &&
                           WiFi.status() != WL_CONNECTED &&
                           canRunBackgroundNetworkTask();
    // Wi-Fi owns automatic reconnect; this minute check is only a fallback.
    // If the worker is busy, skip this round instead of polling every frame.
    if (shouldReconnect && !appNetworkWorker.isBusy()) {
      state.lastWiFiReconnectAttemptTime = now;
      ensureWiFiStationMode();
      WiFi.begin();
    }
  }

  if (startupFlow.phase == STARTUP_IDLE &&
      WiFi.status() == WL_CONNECTED &&
      state.isTimeSynced &&
      hasLocation() &&
      canRunBackgroundNetworkTask()) {
    int currentMin = rtc.getMinute();
    if (currentMin % 10 == 0 && currentMin != state.lastWeatherUpdateMinute &&
        !appNetworkWorker.isBusy()) {
      if (requestWeatherSync(NETWORK_CONTEXT_BACKGROUND)) {
        state.lastWeatherUpdateMinute = currentMin;
      }
    }
  }
  if (state.isWeatherSynced && (now - state.lastSuccessfulWeatherUpdate > WEATHER_EXPIRE_MS)) {
    state.isWeatherSynced = false;
  }

  powerDownWiFiIfIdle();

  static bool wasDark = false;
  if (loadingOverlay.active) state.lastInputTime = now;
  if (state.isAmbientDark && !wasDark) state.lastInputTime = now;
  wasDark = state.isAmbientDark;

  if (!alarmOutputController.isTimeAlarmActive() && !alarmMgr.keepsScreenAwake() && !state.lightLearning && state.isAmbientDark && (now - state.lastInputTime > SCREEN_OFF_TIMEOUT)) {
    if (!state.isScreenOff) { state.isScreenOff = true; u8g2.setPowerSave(1); }
  } else if (state.isScreenOff) {
    state.isScreenOff = false; u8g2.setPowerSave(0);
  }

  if (!state.isScreenOff) displayMgr.updateAndDraw();
  updateBuzzerControl(now);
}
