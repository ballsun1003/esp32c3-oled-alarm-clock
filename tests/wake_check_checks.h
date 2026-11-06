static void completeKeys() {
  CHECK(alarmMgr.showingKeys());
  nowMs += 1900;
  alarmMgr.service(nowMs);
  for (int i = 0; i < 4; i++) {
    alarmMgr.missionInput(false, false, false, nowMs);
    bool right = alarmMgr.keyRight(i);
    alarmMgr.missionInput(!right, right, true, nowMs);
  }
  CHECK(!alarmMgr.missionActive());
}
static void wakeCheckTimeoutChecks() {
  CHECK(AlarmOptions{}.checkTimeoutSec == 60);
  unsigned cases = 0;
  for (int seconds = 10; seconds <= 180; seconds += 10)
    for (bool rollover : {false, true})
      for (bool acknowledge : {false, true}) {
        reset();
        AlarmOptions opt;
        opt.checkMinutes = 1;
        opt.checkTimeoutSec = seconds;
        arm(opt);
        if (rollover) nowMs = 0xffff0000U;
        alarmMgr.acknowledge(nowMs, false);
        nowMs += 59999;
        alarmMgr.service(nowMs);
        CHECK(!alarmOutputController.active);
        nowMs++;
        alarmMgr.service(nowMs);
        CHECK(alarmMgr.checkRinging());
        nowMs += seconds * 1000U - 1;
        alarmMgr.service(nowMs);
        CHECK(alarmMgr.checkRinging());
        if (acknowledge) alarmMgr.acknowledge(nowMs, false);
        nowMs++;
        alarmMgr.service(nowMs);
        CHECK(alarmOutputController.active == !acknowledge);
        CHECK(!alarmMgr.checkRinging());
        if (acknowledge)
          CHECK(!alarmMgr.sessionActive());
        else {
          alarmMgr.acknowledge(nowMs, false);
          nowMs += 60000;
          alarmMgr.service(nowMs);
          CHECK(alarmMgr.checkRinging());
        }
        cases++;
      }
  // Both checks are visible, but only the expired one's missions may restart.
  for (bool reverse : {false, true}) {
    reset();
    AlarmOptions a, b;
    a.checkMinutes = b.checkMinutes = 1;
    a.checkTimeoutSec = 10;
    b.checkTimeoutSec = 180;
    b.mission = 1;
    addAt(reverse ? 1 : 0, 0, a);
    addAt(reverse ? 0 : 1, 0, b);
    alarmMgr.check();
    alarmMgr.acknowledge(nowMs, false);
    completeKeys();
    nowMs += 60000;
    alarmMgr.service(nowMs);
    CHECK(alarmMgr.checkRinging());
    nowMs += 10000;
    alarmMgr.service(nowMs);
    CHECK(!alarmMgr.checkRinging());
    alarmMgr.acknowledge(nowMs, false);
    CHECK(!alarmMgr.missionActive() && !alarmOutputController.active && alarmMgr.sessionActive());
  }
  // A check appearing later must not renew the first check's response deadline.
  reset();
  AlarmOptions a, b;
  a.checkMinutes = 1;
  b.checkMinutes = 2;
  a.checkTimeoutSec = b.checkTimeoutSec = 180;
  b.mission = 1;
  addAt(0, 0, a);
  addAt(1, 0, b);
  alarmMgr.check();
  alarmMgr.acknowledge(nowMs, false);
  completeKeys();
  nowMs += 60000;
  alarmMgr.service(nowMs);
  uint32_t firstStart = nowMs;
  CHECK(alarmMgr.checkRinging());
  nowMs += 60000;
  alarmMgr.service(nowMs);
  CHECK(alarmMgr.checkRinging());
  nowMs = firstStart + 179999;
  alarmMgr.service(nowMs);
  CHECK(alarmMgr.checkRinging());
  nowMs++;
  alarmMgr.service(nowMs);
  CHECK(!alarmMgr.checkRinging());
  alarmMgr.acknowledge(nowMs, false);
  CHECK(!alarmMgr.missionActive() && !alarmOutputController.active && alarmMgr.sessionActive());
  // A normal alarm appearing during a wake check cannot cancel the check's deadline.
  reset();
  a = {};
  a.checkMinutes = 1;
  a.checkTimeoutSec = 180;
  arm(a);
  alarmMgr.acknowledge(nowMs, false);
  nowMs += 60000;
  alarmMgr.service(nowMs);
  firstStart = nowMs;
  CHECK(alarmMgr.checkRinging());
  addAt(1, 1);
  epoch += 60;
  nowMs += 60000;
  alarmMgr.check();
  CHECK(!alarmMgr.checkRinging());
  unsigned rings = alarmOutputController.rings;
  nowMs = firstStart + 179999;
  alarmMgr.service(nowMs);
  CHECK(alarmOutputController.rings == rings);
  nowMs++;
  alarmMgr.service(nowMs);
  CHECK(alarmOutputController.rings == rings + 1);
  reset();
  alarmMgr.alarmCount = 2;
  alarmMgr.options[0].checkTimeoutSec = 0;
  alarmMgr.options[1].checkTimeoutSec = 255;
  alarmMgr.save();
  alarmMgr = AlarmManager{};
  alarmMgr.load();
  CHECK(alarmMgr.options[0].checkTimeoutSec == 10 && alarmMgr.options[1].checkTimeoutSec == 180);
  printf(
      "Wake-check timeout: %u range/ack/deadline/rollover cases, overlapping windows and storage "
      "bounds passed.\n",
      cases);
}
