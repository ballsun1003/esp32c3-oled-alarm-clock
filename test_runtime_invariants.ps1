$ErrorActionPreference = 'Stop'

function Get-Elapsed([uint32]$now, [uint32]$then) {
  return [uint32](([int64]$now - [int64]$then) -band 0xffffffffL)
}

$loopNow = [uint32]100
$newerTimestamp = [uint32]101
if ((Get-Elapsed $loopNow $newerTimestamp) -lt 300000) {
  throw 'The stale-time regression setup did not reproduce the false timeout.'
}
if ((Get-Elapsed $loopNow $loopNow) -ge 300000) {
  throw 'A shared loop timestamp caused a false timeout.'
}

$beforeWrap = [uint32]0xfffffff0L
$afterWrap = [uint32]0x10
if ((Get-Elapsed $afterWrap $beforeWrap) -ne 32) {
  throw 'Rollover-safe elapsed time failed.'
}

$source = Get-Content -LiteralPath "$PSScriptRoot\co2.ino" -Raw
# Arduino emits prototypes at the first free function. Types used by later
# function signatures must already be declared at that insertion point.
$firstFunction = [regex]::Match($source, '(?m)^(?:void|bool|int|String)\s+\w+\([^;\r\n]*\)\s*\{')
$networkResultType = [regex]::Match($source, '(?ms)^struct NetworkResult \{.*?^\};')
if (-not $firstFunction.Success -or -not $networkResultType.Success -or
    $firstFunction.Index -lt $networkResultType.Index + $networkResultType.Length) {
  throw 'Arduino prototype insertion would precede the shared network types.'
}
$checks = @{
  'shared configurable button filter' = 'oled::DebouncedButton filter'
  'edge-only OK button' = 'filter\.update\(digitalRead\(pin\)==LOW,millis\(\),inputSettings,pin!=PIN_BTN_OK\)'
  'shared input timestamp' = 'void processInput\(unsigned long now\)'
  'shared menu timeout timestamp' = 'processInput\(now\);\s*handleMenuInactivity\(now\);'
  'shared reset timestamp' = 'performFullReset\(unsigned long now\)'
  'shared weather timestamp' = 'applyWeatherResult\(const NetworkResult& result, unsigned long now\)'
  'atomic SNTP signal' = 'std::atomic_bool ntpTimeSyncCompleted'
  'non-blocking SNTP apply' = 'time\(&epoch\);\s*localtime_r\(&epoch, &timeinfo\);'
  'UTC system clock' = 'configTzTime\("UTC0", "pool\.ntp\.org"\);'
  'persisted location offset' = 'prefs\.putLong\("locationUtc", rtc\.offset\);'
  'automatic weather timezone' = 'timezone=auto'
  'weather offset parsing' = 'filter\["utc_offset_seconds"\] = true;[\s\S]*?outWeather\.utcOffsetSeconds = utcOffsetSeconds;'
  'local alarm calendar conversion' = 'time_t localMinuteEpoch = minuteEpoch \+ alarmUtcOffset;\s*struct tm minuteTime;\s*gmtime_r\(&localMinuteEpoch, &minuteTime\);'
  'UTC without location' = 'rtc\.offset = 0;\s*state\.cityName\[0\] = ''\\0'';\s*state\.lat = "";\s*state\.lon = "";'
  'protected pending result' = '!busy && !hasQueuedJob && !hasCompletedResult'
  'conservative mutex failure' = 'bool currentBusy = true;'
  'non-blocking worker status' = 'bool readBusyState\(bool includeCompletedResult\)[\s\S]*?xSemaphoreTake\(mutex, 0\)'
  'non-blocking result delivery' = 'bool takeResult\(NetworkResult& result\)[\s\S]*?xSemaphoreTake\(mutex, 0\)'
  'bounded enqueue wait' = '#define NETWORK_ENQUEUE_TIMEOUT_MS 1'
  'active-work reset wait' = 'appNetworkWorker\.hasActiveWork\(\)'
  'guarded Wi-Fi menu' = 'if \(appNetworkWorker\.isBusy\(\)\) \{\s*showInfoOverlay\("Network Busy"\);'
  'rollover-safe overlay deadline' = 'static_cast<int32_t>\(now - loadingOverlay\.expiresAt\) >= 0'
  'editing preserves the navigation box dimensions' = 'queueFocus\(x,y,w,h,state\.isEditing\);'
  'shared outline cursor and filled edit/hold renderer' = 'oled::selection\(u8g2,r,focusFilled,progress\)'
  'WiFi states share one logical page' = 'return view==WIFI_SCANNING \|\| view==WIFI_CONNECTING \? WIFI_LIST : view;'
  'page transitions use logical identity' = 'int frameView=[^;]*: uiPage\(state\.uiState\);'
  'navigation pulses use logical identity' = 'activated && uiPage\(before\)!=uiPage\(state\.uiState\)'
  'WiFi marquee shares logical identity' = 'int focusKey = uiPage\(state\.uiState\) \* 1000 \+ state\.listIdx;'
  'full-screen spinner anchor is fixed' = 'if\(spinner\)drawSpinner\(64,58\);'
  'single scan text is raised independently' = 'drawLoading\("Finding AP\.\.\.", 34, true\);'
  'fixed keyboard box in every mode' = 'static oled::Rect selectorBox\(\) \{ return \{54,33,21,24\}; \}'
  'keyboard action pulses on first navigation frame' = 'textInputMgr\.consumePulse\(\)\)animation\.activate\(TextInputManager::selectorBox\(\)\)'
  'loading acknowledgement cannot delay alarms' = 'if\(alarmMgr\.missionActive\(\) \|\| alarmOutputController\.isTimeAlarmActive\(\)\)\{animation\.cancelActivation\(\);textInputMgr\.consumePulse\(\);\}'
  'scroll editing and hold snap to actual row' = 'scrollView!=page \|\| state\.isEditing \|\| uiHold\.running\(\) \|\| state\.wifiHoldingOk'
  'location menu baselines moved down' = 'int y = 37 \+ row \* 12;'
  'CO2 reserves fixed thousands slot' = 'snprintf\(number,sizeof\(number\),"%4d",state\.co2Limit\);'
  'selected day is inverted' = 'if \(isSelected\) \{\s*u8g2\.setDrawColor\(1\); u8g2\.drawBox\(x \+ 2, y - 8, 26, 10\); u8g2\.setDrawColor\(0\);'
  'day cursor is outlined' = 'if \(state\.daySelectIdx == i\) queueFocus\(x, y - 10, 30, 14\);'
  'full reset is the final main row' = '"Input Setup","Full Reset"'
  'ABC header shows current state' = 'drawPanelHeader\(state\.abcEnabled\?"ABC \[ON\]":"ABC \[OFF\]"\)'
  'input persistence' = 'putUInt\("btnDebounce",inputSettings\.debounceMs\)'
  'jerk persistence' = 'putInt\("moveJerk",motionSettings\.jerk\)'
  'motion defaults require configured hold' = 'beginMenuHold\(state\.motionIdx,now\)'
  'input defaults require configured hold' = 'beginMenuHold\(state\.inputIdx,now\)'
  'wifi delete uses configured hold' = 'beginMenuHold\(state\.listIdx,now\)'
  'shared hold duration uses input setting' = 'uiHold\.begin\(state\.uiState\*1000\+item,now,inputSettings\.holdDelayMs\)'
  'repeat delay has separate persistence' = 'p\.putUInt\("btnRepeatWait",inputSettings\.repeatDelayMs\)'
  'keyboard repeat uses separate delay' = 'repeatDelay = inputSettings\.repeatDelayMs;'
  'startup skip uses configured hold' = 'uiHold\.begin\(-2,now,inputSettings\.holdDelayMs\)'
  'full reset duration is fixed at three seconds' = '#define FULL_RESET_HOLD_MS 3000'
  'both reset confirmations require a rendered three-second hold' = 'uiHold\.begin\(RESET_CONFIRM\*1000\+state\.resetConfirmStep,now,FULL_RESET_HOLD_MS\);\s*if\(uiHold\.poll\(okPressed,now\)\)'
  'first reset confirmation advances to second with No selected' = 'if\(state\.resetConfirmStep==0\) \{\s*state\.resetConfirmStep=1;\s*state\.resetConfirmYes=false;\s*\} else performFullReset\(now\);'
  'reset entry starts at first confirmation' = 'cancelMenuHold\(\);\s*state\.resetConfirmStep = 0;\s*state\.resetConfirmYes = false;\s*state\.uiState = RESET_CONFIRM;'
  'wifi delete waits for a rendered hold' = 'uiHold\.poll\(true,now\)'
  'five motion rows' = 'state\.motionIdx>=state\.motionOffset\+5'
  'five input rows' = 'state\.inputIdx>=state\.inputOffset\+5'
  'five alarm option rows' = 'state\.optionIdx>=state\.optionOffset\+5'
  'check response limit uses each alarm deadline' = 'runs\[i\]\.phase == CHECK_ACTIVE && uint32_t\(now-runs\[i\]\.started\) >= options\[i\]\.checkTimeoutSec\*1000UL'
  'check response window starts when shown' = 'runs\[i\]\.phase = CHECK_ACTIVE; runs\[i\]\.started = now;'
  'reconnect check keeps minute cadence' = 'if \(now - lastMin > 60000\) \{\s*lastMin = now;[\s\S]*?if \(shouldReconnect && !appNetworkWorker\.isBusy\(\)\)'
  'alarm-list chord toggle' = 'case ALARM_LIST:[\s\S]*?if \(bothHeld\)[\s\S]*?alarm\.enabled = !alarm\.enabled;\s*alarmMgr\.save\(\);'
  'alarm-list chord grace' = 'now - alarmListFirstPressTime > DUAL_BUTTON_GRACE_MS'
  'alarm-list delayed tap' = 'if \(alarmListPendingBtn != 0 && !alarmListActionExecuted\) \{\s*navigation = alarmListPendingBtn;'
  'dedicated alarm output task' = 'xTaskCreate\(taskEntry, "alarmOutput", ALARM_OUTPUT_TASK_STACK_SIZE'
  'time alarm level drive' = 'bool timeAlarmOn = \(now - timeAlarmCycleStart\) % TIME_ALARM_PERIOD_MS < TIME_ALARM_ON_MS;\s*digitalWrite\(PIN_BUZZER, timeAlarmOn \? HIGH : LOW\);'
  'calendar scans during other alarm sessions' = 'void check\(\) \{\s*if \(!state\.isTimeSynced\) return;'
  'alarm activation reaches output controller' = 'void ringAgain\(unsigned long now, bool checkOnly = false\)[\s\S]*?alarmOutputController\.startTimeAlarm\(now\);'
  'CO2 uses controller-owned LEDC' = 'ledcAttach\(PIN_BUZZER, CO2_TONE_FREQUENCY_HZ, 10\)[\s\S]*?ledcWriteTone\(PIN_BUZZER, CO2_TONE_FREQUENCY_HZ\)'
  'CO2 fails loud when LEDC fails' = 'if \(!startCo2Tone\(\)\) digitalWrite\(PIN_BUZZER, HIGH\);\s*setActiveMode\(BUZZER_CO2_ALERT\);'
  'alarm output task fallback' = 'if \(workerHandle == nullptr\) service\(now\);'
  'timezone change updates alarm calendar' = 'rtc\.offset = result\.utcOffsetSeconds;\s*alarmMgr\.setCalendarOffset\(result\.utcOffsetSeconds\);'
  'one-shot consumption is deferred' = 'if \(alarms\[i\]\.days == 0\) pendingOneShotConsumption\[i\] = true;'
  'dismissal consumes pending one-shots' = 'void dismissCurrentAlarm\(\) \{\s*alarmOutputController\.dismissTimeAlarm\(\);[\s\S]*?alarms\[i\]\.enabled = false;[\s\S]*?if \(oneShotAlarmChanged\) save\(\);'
  'preexisting alarm-dismiss input needs release' = 'alarmDismissRequiresRelease = okPressed \|\| bothHeld;[\s\S]*?if \(alarmDismissRequiresRelease\) \{\s*if \(!okPressed && !bothHeld\) alarmDismissRequiresRelease = false;\s*return;'
  'location failure adopts displayed timezone' = 'if \(!weatherApplied\) alarmMgr\.setCalendarOffset\(rtc\.offset\);'
  'CO2 mute follows request state' = 'void muteCo2AlertIfRequested\(\) \{\s*if \(isCo2AlertRequested\(\)\) state\.isCo2Muted = true;'
}

foreach ($check in $checks.GetEnumerator()) {
  if ($source -notmatch $check.Value) { throw "Missing $($check.Key)." }
}

if ($source -match '\b(?:tone|noTone)\s*\(') {
  throw 'The asynchronous Arduino Tone queue returned to the alarm output path.'
}
if ($source.Contains('state.isAlarmRinging')) {
  throw 'A duplicate time-alarm state bypasses the output controller.'
}

$alarmCheck = [regex]::Match($source, 'void check\(\) \{[\s\S]*?\n  \}\n\} alarmMgr;').Value
if ($alarmCheck -match 'enabled\s*=\s*false') {
  throw 'A one-shot alarm is consumed before the user dismisses it.'
}

if ([regex]::Matches($source, 'DUAL_BUTTON_GRACE_MS').Count -lt 3) {
  throw 'Text input and alarm list do not share the dual-button grace interval.'
}

$removedLocationResidue = @(
  'useManualLocation',
  'currentCity',
  'NETWORK_CONTEXT_LOCATION_AUTO',
  'NETWORK_CONTEXT_LOCATION_MANUAL',
  'NETWORK_CONTEXT_LOCATION_SEARCH',
  'resolveCityName',
  '[Manual]',
  'manualLoc',
  'curCity'
)
foreach ($removed in $removedLocationResidue) {
  if ($source.Contains($removed)) { throw "Legacy location residue remains: $removed" }
}

$legacyLocationStorage = @(
  'prefs\.(get|put)String\("city"',
  'prefs\.(get|put)String\("lat"',
  'prefs\.(get|put)String\("lon"',
  'prefs\.(get|put)String\("curCity"',
  'prefs\.(get|put)Bool\("manualLoc"',
  'prefs\.(get|put)Long\("utcOffset"'
)
foreach ($pattern in $legacyLocationStorage) {
  if ($source -match $pattern) { throw "Legacy location storage path remains: $pattern" }
}

'Runtime invariant checks passed.'
