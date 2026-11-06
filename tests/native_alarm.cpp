#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../oled_motion.h"
#include "../oled_input.h"
#define CHECK(c)                                           \
  do {                                                     \
    if (!(c)) {                                            \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
      exit(1);                                             \
    }                                                      \
  } while (0)
using std::max;
using std::min;
using String = std::string;
constexpr int MAX_ALARMS = 100, ALARM_LOOKBACK_MINUTES = 60;
uint32_t nowMs = 1000;
time_t epoch = 1788678000;  // Tests use the computed UTC tm, not local TZ.
unsigned long millis() { return nowMs; }
uint32_t esp_random() { return 9; }
time_t test_time(time_t* out) {
  if (out) *out = epoch;
  return epoch;
}
void gmtime_r(const time_t* value, tm* result) { gmtime_s(result, value); }
template <class T>
T clampValue(T n, T lo, T hi) {
  return std::max(lo, std::min(hi, n));
}
struct Alarm {
  uint8_t hour, minute, days;
  bool enabled;
};
#include "../_backup/alarm-options-test.h"
struct {
  bool isTimeSynced = true, isScreenOff = false, abcEnabled = true, wifiAutoReconnectEnabled = true;
  int co2Limit = 1500, filteredCds = 4095;
  char cityName[32] = "";
  String lat, lon;
  unsigned long alarmStartTime = 0;
} state;
struct {
  long offset = 0;
} rtc;
oled::MotionSettings motionSettings;
oled::InputSettings inputSettings;
bool hasValidLatLon(const String& a, const String& b) { return !a.empty() && !b.empty(); }
void copyToBuffer(char* out, size_t size, const String& in) {
  snprintf(out, size, "%s", in.c_str());
}
struct Prefs {
  std::map<String, std::vector<uint8_t>> bytes;
  std::map<String, long> nums;
  std::map<String, String> strings;
  bool begin(const char*, bool) { return true; }
  void end() {}
  unsigned getUInt(const char* k, unsigned d) { return nums.count(k) ? nums[k] : d; }
  int getInt(const char* k, int d) { return nums.count(k) ? nums[k] : d; }
  long getLong(const char* k, long d) { return nums.count(k) ? nums[k] : d; }
  bool getBool(const char* k, bool d) { return nums.count(k) ? nums[k] : d; }
  String getString(const char* k, const char* d) { return strings.count(k) ? strings[k] : d; }
  size_t getBytesLength(const char* k) { return bytes[k].size(); }
  size_t getBytes(const char* k, void* out, size_t size) {
    auto& b = bytes[k];
    size = min(size, b.size());
    memcpy(out, b.data(), size);
    return size;
  }
  void putUInt(const char* k, unsigned v) { nums[k] = v; }
  void putInt(const char* k, int v) { nums[k] = v; }
  void putLong(const char* k, long v) { nums[k] = v; }
  void putBool(const char* k, bool v) { nums[k] = v; }
  void putString(const char* k, const String& v) { strings[k] = v; }
  void putBytes(const char* k, const void* v, size_t n) {
    bytes[k] = std::vector<uint8_t>((const uint8_t*)v, (const uint8_t*)v + n);
  }
  void remove(const char* k) { bytes.erase(k); }
} prefs;
struct Preferences {
  bool begin(const char*,bool){return true;}
  void end(){}
  void putUInt(const char* k,unsigned value){prefs.putUInt(k,value);}
};
#include "../_backup/input-save.h"
struct {
  bool active = false;
  unsigned rings = 0;
  bool isTimeAlarmActive() { return active; }
  void startTimeAlarm(unsigned long now) {
    active = true;
    state.alarmStartTime = now;
    rings++;
  }
  void dismissTimeAlarm() { active = false; }
} alarmOutputController;
struct {
  void setPowerSave(int) {}
} u8g2;
#define time test_time
#include "../_backup/alarm-manager.h"
#undef time
void reset() {
  alarmMgr = AlarmManager{};
  alarmOutputController.active = false;
  state.isTimeSynced = true;
  nowMs = 1000;
  epoch = 1788678000;
  state.filteredCds = 4095;
}
void arm(AlarmOptions options = {}) {
  tm t;
  gmtime_r(&epoch, &t);
  alarmMgr.alarmCount = 1;
  alarmMgr.alarms[0] = {uint8_t(t.tm_hour), uint8_t(t.tm_min), 0, true};
  alarmMgr.options[0] = options;
  alarmMgr.check();
  CHECK(alarmOutputController.active);
  CHECK(alarmMgr.alarms[0].enabled);
}
void addAt(int index, int minutes, AlarmOptions options = {}) {
  time_t at = epoch + minutes * 60;
  tm t;
  gmtime_r(&at, &t);
  alarmMgr.alarms[index] = {uint8_t(t.tm_hour), uint8_t(t.tm_min), 0, true};
  alarmMgr.options[index] = options;
  alarmMgr.alarmCount = max(alarmMgr.alarmCount, index + 1);
}
void minutesPass(int minutes) {
  for (int i = 0; i < minutes; i++) {
    epoch += 60;
    nowMs += 60000;
    alarmMgr.service(nowMs);
    alarmMgr.check();
  }
}
void overlappingAlarms() {
  // B must ring at its scheduled minute and leave A's original snooze intact.
  reset();
  AlarmOptions a;
  a.snoozeMinutes = 5;
  a.snoozeCount = 1;
  arm(a);
  addAt(1, 1);
  alarmMgr.acknowledge(nowMs, true);
  minutesPass(1);
  CHECK(alarmOutputController.active);
  alarmMgr.acknowledge(nowMs, false);
  CHECK(alarmMgr.alarms[0].enabled && !alarmMgr.alarms[1].enabled);
  CHECK(alarmMgr.sessionActive());
  minutesPass(3);
  CHECK(!alarmOutputController.active);
  minutesPass(1);
  CHECK(alarmOutputController.active);
  alarmMgr.acknowledge(nowMs, true);  // A already used its one snooze.
  CHECK(!alarmMgr.sessionActive() && !alarmMgr.alarms[0].enabled);

  // The same isolation applies to quiet wake checks, including no NTP sync.
  reset();
  a = {};
  a.checkMinutes = 10;
  arm(a);
  addAt(1, 1);
  alarmMgr.acknowledge(nowMs, false);
  minutesPass(1);
  CHECK(alarmOutputController.active && !alarmMgr.checkRinging());
  alarmMgr.acknowledge(nowMs, false);
  CHECK(alarmMgr.sessionActive());
  state.isTimeSynced = false;
  minutesPass(8);
  CHECK(!alarmOutputController.active);
  minutesPass(1);
  CHECK(alarmMgr.checkRinging());
  alarmMgr.acknowledge(nowMs, false);
  CHECK(!alarmMgr.sessionActive());

  // The former 60-minute lookback loss: five 20-minute snoozes, B at minute 1.
  reset();
  a = {};
  a.snoozeMinutes = 20;
  a.snoozeCount = 5;
  arm(a);
  addAt(1, 1);
  alarmMgr.acknowledge(nowMs, true);
  for (int minute = 1; minute <= 100; minute++) {
    minutesPass(1);
    CHECK(alarmOutputController.active == (minute == 1 || minute % 20 == 0));
    if (minute == 1) alarmMgr.acknowledge(nowMs, false);
    if (minute % 20 == 0) alarmMgr.acknowledge(nowMs, minute < 100);
    if (minute >= 1) CHECK(!alarmMgr.alarms[1].enabled);
  }
  CHECK(!alarmMgr.sessionActive() && !alarmMgr.alarms[0].enabled);

  // New scheduled alarms are also observed during ringing and both missions.
  for (int phase = 0; phase < 4; phase++) {
    reset();
    a = {};
    a.mission = phase == 1 ? 2 : phase >= 2 ? 1 : 0;
    a.lightRaw = 1000;
    arm(a);
    addAt(1, 1);
    if (phase) alarmMgr.acknowledge(nowMs, false);
    if (phase == 3) {
      nowMs += 1900;
      alarmMgr.service(nowMs);
      CHECK(!alarmMgr.showingKeys());
    }
    unsigned rings = alarmOutputController.rings;
    minutesPass(1);
    CHECK(alarmOutputController.active);
    CHECK(alarmOutputController.rings == rings + 1);
    alarmMgr.check();
    CHECK(alarmOutputController.rings == rings + 1);
    alarmMgr.acknowledge(nowMs, false);
    if (phase)
      CHECK(alarmMgr.missionActive());
    else
      CHECK(!alarmMgr.alarms[0].enabled && !alarmMgr.alarms[1].enabled);
  }
  // A visible check and a new regular alarm share the sound, not check missions.
  reset();
  a = {};
  a.checkMinutes = 1;
  a.lightRaw = 100;
  a.mission = 0;
  arm(a);
  addAt(1, 1);
  alarmMgr.acknowledge(nowMs, false);
  minutesPass(1);
  CHECK(alarmOutputController.active && !alarmMgr.checkRinging());
  alarmMgr.acknowledge(nowMs, false);
  CHECK(!alarmMgr.sessionActive());

  // Full capacity: all 100 independent sleepers must wake at their deadlines.
  reset();
  a = {};
  a.snoozeCount = 1;
  a.snoozeMinutes = 20;
  for (int i = 0; i < 100; i++) {
    a.snoozeMinutes = 1 + i % 20;
    addAt(i, 0, a);
  }
  alarmMgr.check();
  alarmMgr.acknowledge(nowMs, true);
  for (int minute = 1; minute <= 20; minute++) {
    minutesPass(1);
    CHECK(alarmOutputController.active);
    alarmMgr.acknowledge(nowMs, false);
    for (int i = 0; i < 100; i++) CHECK(alarmMgr.alarms[i].enabled == (1 + i % 20 > minute));
  }
  CHECK(!alarmMgr.sessionActive());
}
void coincidentDeadlines() {
  // Three sources at the same deadline: A's snooze, B's check, C's calendar.
  // The event-loop order must not change which one-shots are consumed.
  for (bool serviceFirst : {false, true}) {
    reset();
    AlarmOptions a, b;
    a.snoozeMinutes = 2; a.snoozeCount = 1; a.checkMinutes = 1;
    b.checkMinutes = 1;
    arm(a); addAt(1, 1, b); addAt(2, 2);
    alarmMgr.acknowledge(nowMs, true);
    minutesPass(1); CHECK(alarmOutputController.active);
    alarmMgr.acknowledge(nowMs, false);
    CHECK(alarmMgr.alarms[0].enabled && !alarmMgr.alarms[1].enabled);
    epoch += 60; nowMs += 60000;
    if (serviceFirst) alarmMgr.service(nowMs);
    alarmMgr.check();
    if (!serviceFirst) alarmMgr.service(nowMs);
    CHECK(alarmOutputController.active && !alarmMgr.checkRinging());
    alarmMgr.acknowledge(nowMs, false);
    for (int i = 0; i < 3; i++) CHECK(!alarmMgr.alarms[i].enabled);
    minutesPass(1); CHECK(alarmMgr.checkRinging());
    alarmMgr.acknowledge(nowMs, false); CHECK(!alarmMgr.sessionActive());
  }
  // Multiple check deadlines spanning millis() rollover stay independent.
  reset(); nowMs=0xffff0000U;
  for (int i=0; i<100; i++) {
    AlarmOptions a; a.checkMinutes=1+i%10; addAt(i,0,a);
  }
  alarmMgr.check(); alarmMgr.acknowledge(nowMs,false);
  for (int minute=1;minute<=10;minute++) {
    minutesPass(1); CHECK(alarmMgr.checkRinging());
    alarmMgr.acknowledge(nowMs,false);
    CHECK(alarmMgr.sessionActive()==(minute<10));
  }
  puts("Alarm overlap regressions passed: independent snoozes/checks, 100-minute wait, "
       "missions, simultaneous deadlines in both orders, 100 sleepers/checks and rollover.");
}
void lightOptionMerging() {
  // Every order of a light alarm and an alarm with a disabled light mission.
  for (int order = 0; order < 2; order++)
    for (int otherMission = 0; otherMission < 4; otherMission++) {
      reset();
      AlarmOptions light, other;
      light.mission = 2;
      light.lightRaw = 2000;
      other.mission = otherMission;
      other.lightRaw = 100;
      addAt(order, 0, light);
      addAt(1 - order, 0, other);
      alarmMgr.check();
      alarmMgr.acknowledge(nowMs, false);
      CHECK(alarmMgr.lightMission());
      CHECK(alarmMgr.lightTarget() == (otherMission & 2 ? 100 : 2000));
    }
  // An invalid/unlearned light target must not auto-complete a light mission.
  reset();
  AlarmOptions light, bad;
  light.mission = 2;
  light.lightRaw = 2000;
  bad.mission = 2;
  bad.lightRaw = 0xffff;
  addAt(0, 0, bad);
  addAt(1, 0, light);
  alarmMgr.check();
  alarmMgr.acknowledge(nowMs, false);
  CHECK(alarmMgr.lightTarget() == 2000);
}
#include "wake_check_checks.h"
int main() {
  prefs.nums.erase("btnHold");
  prefs.nums.erase("btnRepeatWait");
  alarmMgr.load();
  CHECK(inputSettings.holdDelayMs==1000);
  CHECK(inputSettings.repeatDelayMs==600);
  for(unsigned delay=300;delay<=3000;delay+=50) {
    inputSettings.holdDelayMs=delay;
    saveInputSettings();
    inputSettings.holdDelayMs=0;
    alarmMgr.load();
    CHECK(inputSettings.holdDelayMs==delay);
  }
  for(unsigned invalid : {0U,299U,3001U,65535U}) {
    prefs.putUInt("btnHold",invalid);
    alarmMgr.load();
    CHECK(inputSettings.holdDelayMs==(invalid<300?300:3000));
  }
  for(unsigned hold : {300U,1000U,3000U})
    for(unsigned delay=200;delay<=1000;delay+=50) {
      inputSettings.holdDelayMs=hold;
      inputSettings.repeatDelayMs=delay;
      saveInputSettings();
      inputSettings={};
      alarmMgr.load();
      CHECK(inputSettings.holdDelayMs==hold && inputSettings.repeatDelayMs==delay);
    }
  for(unsigned invalid : {0U,199U,1001U,65535U}) {
    prefs.putUInt("btnRepeatWait",invalid);
    alarmMgr.load();
    CHECK(inputSettings.repeatDelayMs==(invalid<200?200:1000));
    CHECK(inputSettings.holdDelayMs==3000);
  }
  inputSettings=oled::InputSettings{};saveInputSettings();
  puts("Repeat wait: independent 600ms default, 51 hold/wait save-load combinations and bounds passed.");
  puts("Hold settings: default 1000ms, all 55 saved values and storage bounds passed.");
  wakeCheckTimeoutChecks();
  overlappingAlarms();
  coincidentDeadlines();
  lightOptionMerging();
  reset();
  arm();
  alarmMgr.acknowledge(nowMs, false);
  CHECK(!alarmOutputController.active);
  CHECK(!alarmMgr.sessionActive());
  CHECK(!alarmMgr.alarms[0].enabled);
  alarmMgr.check();
  CHECK(!alarmOutputController.active);
  reset();
  AlarmOptions o;
  o.snoozeMinutes = 1;
  o.snoozeCount = 1;
  arm(o);
  alarmMgr.acknowledge(nowMs, true);
  CHECK(!alarmOutputController.active);
  CHECK(alarmMgr.alarms[0].enabled);
  nowMs += 59999;
  alarmMgr.service(nowMs);
  CHECK(!alarmOutputController.active);
  nowMs++;
  alarmMgr.service(nowMs);
  CHECK(alarmOutputController.active);
  alarmMgr.acknowledge(nowMs, true);
  CHECK(!alarmMgr.sessionActive());
  CHECK(!alarmMgr.alarms[0].enabled);
  reset();
  o = {};
  o.mission = 2;
  o.lightRaw = 1000;
  arm(o);
  alarmMgr.acknowledge(nowMs, false);
  CHECK(alarmMgr.lightMission());
  state.filteredCds = 900;
  alarmMgr.service(nowMs);
  nowMs += 2999;
  alarmMgr.service(nowMs);
  CHECK(alarmMgr.missionActive());
  state.filteredCds = 1200;
  alarmMgr.service(nowMs);
  nowMs += 5000;
  state.filteredCds = 900;
  alarmMgr.service(nowMs);
  nowMs += 3000;
  alarmMgr.service(nowMs);
  CHECK(!alarmMgr.sessionActive());
  reset();
  o = {};
  o.mission = 1;
  o.checkMinutes = 1;
  arm(o);
  alarmMgr.acknowledge(nowMs, false);
  CHECK(alarmMgr.showingKeys());
  nowMs += 1900;
  alarmMgr.service(nowMs);
  for (int i = 0; i < 4; i++) {
    alarmMgr.missionInput(false, false, false, nowMs);
    bool right = alarmMgr.keyRight(i);
    alarmMgr.missionInput(!right, right, true, nowMs);
  }
  CHECK(!alarmMgr.missionActive());
  CHECK(alarmMgr.sessionActive());
  nowMs += 60000;
  alarmMgr.service(nowMs);
  CHECK(alarmMgr.checkRinging());
  nowMs += 60000;
  alarmMgr.service(nowMs);
  CHECK(alarmOutputController.active);
  CHECK(!alarmMgr.checkRinging());
  reset();
  o = {};
  o.snoozeMinutes = 1;
  o.snoozeCount = 1;
  arm(o);
  nowMs = 0xfffffff0U;
  alarmMgr.acknowledge(nowMs, true);
  nowMs += 60000;
  alarmMgr.service(nowMs);
  CHECK(alarmOutputController.active);
  reset();
  alarmMgr.alarmCount = 100;
  for (int i = 0; i < 100; i++) {
    alarmMgr.alarms[i] = {uint8_t(i % 24), uint8_t(i % 60), uint8_t(i % 128), bool(i % 2)};
    alarmMgr.options[i] = {uint8_t(1 + i % 20), uint8_t(i % 6), uint8_t(i % 11), uint8_t(i % 4),
                           uint16_t(i * 40),uint8_t(10+(i%18)*10)};
  }
  alarmMgr.save();
  alarmMgr = AlarmManager{};
  alarmMgr.load();
  CHECK(alarmMgr.alarmCount == 100);
  CHECK(alarmMgr.options[99].lightRaw == 3960);
  CHECK(alarmMgr.options[99].mission == 3);
  for(int i=0;i<100;i++)CHECK(alarmMgr.options[i].checkTimeoutSec==10+(i%18)*10);
  CHECK(alarmMgr.alarms[99].hour == 3);
  puts(
      "Actual alarm manager checks passed: one-shot, snooze limit, light hold reset, keys, recheck "
      "fallback, rollover and 100-record persistence.");
}
