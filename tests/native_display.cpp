#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>
#include <assert.h>
#undef assert
#define assert(c) do { if(!(c)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);} } while(0)

#include "../oled_ui.h"
#include "../oled_widgets.h"
#include "../oled_input.h"
#include "../_backup/ui-enums.h"
#include "../_backup/reset-constant.h"
#include "u8g2.h"
using std::max;
using std::min;
uint32_t displayTime = 12345;
bool settleFocus=false;
unsigned long millis() { return displayTime; }
class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
  void remove(size_t at,size_t count=std::string::npos){erase(at,count);}
  void toCharArray(char* dest,size_t n)const{snprintf(dest,n,"%s",c_str());}
};
struct {bool pressed=false;bool down()const{return pressed;}} btnLeft,btnRight;
#define DUAL_BUTTON_GRACE_MS inputSettings.chordMs
void copyToBuffer(char* to, size_t n, const char* from) { snprintf(to, n, "%s", from); }
void copyToBuffer(char* to,size_t n,const String& from){copyToBuffer(to,n,from.c_str());}
template<class T>T clampValue(T v,T lo,T hi){return max(lo,min(v,hi));}
constexpr int MAIN_ITEMS=10, MOTION_ITEMS=8, INPUT_ITEMS=7, ALARM_OPTION_ITEMS=6;
struct Alarm {
  unsigned char hour = 23, minute = 59, days = 127;
  bool enabled = false;
};
#include "../_backup/display-alarm-options-test.h"
struct {
  Alarm tempAlarm;
  AlarmOptions tempAlarmOptions={20,5,10,3,65535,60};
  int editRow = 0, daySelectIdx = 7, co2Limit = 3000, optionIdx = 3, snoozeIdx = 0, lightIdx = 1,
      motionIdx = 1, motionOffset = 0, inputIdx=1,inputOffset=0,menuIdx=0,menuOffset=0,valueDirection=1;
  UIState uiState=ALARM_EDIT;
  bool abcEnabled=true,abcConfirmTargetOn=false,abcConfirmYes=false;
  bool isEditing = false, lightLearning = false;
  int filteredCds = 0;
  unsigned long lightLearnStart = 0;
  unsigned long lastWeatherInfoCycleChange=0;
  bool isWeatherSynced=true,isSensorError=false;
  float tempOut=-12.3f,windSpeed=12.3f,pm10=123;
  int weatherCode=0,isDay=1,rainProb=100,windDir=45;
  int listIdx=0,listOffset=0,locMenuIdx=0,optionOffset=0,lightSampleCount=0;
  unsigned long lightLearnSample=0;
  bool wifiHoldingOk=false,wifiLongHandled=false,resetConfirmYes=false;
  int resetConfirmStep=0;
  int wifiListReturnCategory=0;
} state;
struct {Alarm alarms[100];int alarmCount=4;} alarmMgr;
struct APItem {String ssid,bssid;int rssi;bool isSecured;int category;bool hasDuplicate;};
struct {
  std::vector<APItem> apList;char connectingSSID[33]={};int connectStatus=0,scanMessage=0;bool showFailMessage=false,isScanning=false;
  int scans=0;
  void startScan(){scans++;}
  void releaseList(){apList.clear();}
  void buildAndSortList(int){}
  int findIndexByCategory(int category){for(int i=0;i<apList.size();i++)if(apList[i].category==category)return i;return 0;}
  uint16_t getSignalIcon(int rssi){return rssi==-100?57879:rssi<-80?57880:rssi<-60?57881:57882;}
} apMgr;
constexpr int WIFI_SCAN_MESSAGE_NONE=0,WIFI_SCAN_MESSAGE_DISCONNECTED=1,WIFI_SCAN_MESSAGE_CONNECTION_FAILED=2,WIFI_OFF=0;
struct {int getMode(){return 1;}void scanDelete(){}void disconnect(bool,bool){}} WiFi;
struct {int removed=0;void remove(const char*){removed++;}bool getPassword(const char*,char*){return false;}} wifiStorage;
void applyWiFiAutoReconnectPreference(bool){}
void disconnectCurrentWiFiAndScan(){}
int wifiConnectionAttempts=0;
void beginWiFiConnectionAttempt(const String&,const char*){wifiConnectionAttempts++;}
struct LocationResult{String name,country,lat,lon;};
std::vector<LocationResult> locResults;
oled::MotionSettings motionSettings;
oled::InputSettings inputSettings;
oled::HoldGesture uiHold;
#include "../_backup/display-helpers.h"
int inputSaves=0,motionSaves=0;
void saveInputSettings(){inputSaves++;}
void saveMotionSettings(){motionSaves++;}
void showInfoOverlay(const char*){}
int fullResetCalls=0;
unsigned long fullResetAt=0;
void performFullReset(unsigned long now){fullResetCalls++;fullResetAt=now;}
struct Canvas {
  u8g2_t g = {};
  uint8_t bytes[1024] = {};
  uint8_t panel[1024]={};unsigned transferred=0;
  u8x8_display_info_t info = {};
  int x = 0, y = 0;
  Canvas() {
    info.tile_width = 16;
    info.tile_height = 8;
    info.pixel_width = 128;
    info.pixel_height = 64;
    g.u8x8.display_info = &info;
    u8g2_SetupBuffer(&g, bytes, 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_SetFontMode(&g, 1);
  }
  void setFont(const uint8_t* f) { u8g2_SetFont(&g, f); }
  int getStrWidth(const char* s) { return u8g2_GetStrWidth(&g, s); }
  int getXOffsetGlyph(uint16_t code) { return u8g2_GetXOffsetGlyph(&g,code); }
  void setDrawColor(int c) { u8g2_SetDrawColor(&g, c); }
  void setClipWindow(int x0,int y0,int x1,int y1){u8g2_SetClipWindow(&g,x0,y0,x1,y1);}
  void setMaxClipWindow(){u8g2_SetMaxClipWindow(&g);}
  void setCursor(int xx, int yy) {
    x = xx;
    y = yy;
  }
  void drawStr(int xx, int yy, const char* s) { u8g2_DrawStr(&g, xx, yy, s); }
  void print(const char* s) { x += u8g2_DrawStr(&g, x, y, s); }
  void print(const String& s){print(s.c_str());}
  void print(unsigned n) {
    char s[24];
    snprintf(s, sizeof(s), "%u", n);
    print(s);
  }
  void printf(const char* fmt, ...) {
    char s[128];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof(s), fmt, a);
    va_end(a);
    print(s);
  }
  void drawLine(int x1, int y1, int x2, int y2) { u8g2_DrawLine(&g, x1, y1, x2, y2); }
  void drawBox(int xx, int yy, int w, int h) { u8g2_DrawBox(&g, xx, yy, w, h); }
  void drawFrame(int xx, int yy, int w, int h) { u8g2_DrawFrame(&g, xx, yy, w, h); }
  void drawHLine(int xx, int yy, int w) { u8g2_DrawHLine(&g, xx, yy, w); }
  void drawPixel(int xx,int yy){u8g2_DrawPixel(&g,xx,yy);}
  void drawGlyph(int xx,int yy,uint16_t code){u8g2_DrawGlyph(&g,xx,yy,code);}
  bool lit(int xx, int yy) { return bytes[(yy >> 3) * 128 + xx] & (1 << (yy & 7)); }
  void clear() {
    memset(bytes, 0, 1024);
    setDrawColor(1);
  }
  void clearBuffer(){clear();}
  uint8_t* getBufferPtr(){return bytes;}
  void sendBuffer(){memcpy(panel,bytes,1024);transferred+=1024;}
  void updateDisplayArea(int x,int y,int w,int h){
    assert(x>=0&&x+w<=16&&y>=0&&y+h<=8);
    for(int row=y;row<y+h;row++){memcpy(panel+row*128+x*8,bytes+row*128+x*8,w*8);transferred+=w*8;}
  }
} u8g2;
#include "../_backup/text-input.h"
void setupInput(bool u,bool d,bool e,bool okPressed,unsigned long now){
  bool userNavigation=e;
  switch(state.uiState){
#include "../_backup/setup-input.h"
#include "../_backup/reset-input.h"
#include "../_backup/wifi-input.h"
  default:break;
  }
}
class Display {
 public:
  oled::Rect focus = {};
  bool outlined = false, queued = false;
  oled::Rect& focusRect = focus;
  bool& focusFilled = outlined;
  bool& focusRequested = queued;
  oled::UiAnimation animation;
  oled::RollingText rolling;
  uint32_t marqueeFrameSerial=0,marqueeLastSeenFrame=0;
  int marqueeFocusKey=-1;unsigned long marqueeFocusStartTime=0;
  void getTimeDisplayText(char* text,size_t n){copyToBuffer(text,n,"23:59");}
  void getDateDisplayText(char* text,size_t n){copyToBuffer(text,n,"09/07 Mon");}
  int scrollView=-1,scrollTarget=0;float scrollFrom=0;unsigned long scrollStarted=0;
  void getLocationSummaryLine(char* text,size_t n){copyToBuffer(text,n,"Gypsy City, Country");}
  void queueFocus(int x, int y, int w, int h, bool outline = false) {
    focus = {x, y, w, h};
    outlined = outline;
    queued = true;
  }
#include "../_backup/display-methods.h"
  void verify(const char* screen, bool padding = true) {
    if (!queued) return;
    auto r = focus;
    if (!oled::inside(r)) {
      fprintf(stderr, "Outside focus: %s\n", screen);
      exit(1);
    }
    if (padding)
      for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
          bool near = x <= r.x + 1 || x >= r.x + r.w - 2 || y <= r.y + 1 || y >= r.y + r.h - 2;
          if (near && u8g2.lit(x, y)) {
            fprintf(stderr, "Text needs padding: %s field %d at %d,%d box %d,%d %dx%d\n", screen,
                    state.editRow, x, y, r.x, r.y, r.w, r.h);
            exit(1);
          }
        }
  }
  void finish() {
    if(settleFocus){animation=oled::UiAnimation{};scrollView=-1;}
    finishFocus(millis());
    queued = false;
  }
} display;
void save(const char* name) {
  char path[160];
  snprintf(path, sizeof(path), "_backup/%s.pbm", name);
  FILE* f = fopen(path, "wb");
  fprintf(f, "P1\n128 64\n");
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) fprintf(f, "%d ", u8g2.lit(x, y) ? 0 : 1);
    fputs("\n", f);
  }
  fclose(f);
}
#include "new_ui_checks.h"
#include "consistency_checks.h"
#include "glyph_integrity_checks.h"
#include "wake_check_ui_checks.h"
#include "hold_ui_checks.h"
#include "repeat_delay_checks.h"
int main() {
  repeatDelayChecks();
  holdUiChecks();
  wakeCheckUiChecks();
  co2GlyphChecks();
  co2AnimationIntegrity();
  settingBracketChecks();
  bracketAnimationIntegrity();
  commonAlignmentIntegrity();
  // Exercise the firmware's actual focus compositor, not a static replica.
  unsigned animatedFrames = 0;
  for (int from = 0; from < 8; from++)
    for (int to = 0; to < 8; to++) {
      display.animation = oled::UiAnimation{};
      state.isEditing = false;
      state.editRow = from;
      displayTime = 1000;
      u8g2.clear();
      display.drawAlarmEdit();
      display.finish();
      for (int ms = 0; ms <= 120; ms++) {
        displayTime = 1200 + ms;
        state.editRow = to;
        u8g2.clear();
        display.drawAlarmEdit();
        display.finish();
        auto r = display.animation.current(displayTime);
        if (!oled::inside(r)) {
          fputs("Animated alarm focus outside OLED\n", stderr);
          return 1;
        }
        animatedFrames++;
      }
      // Reverse direction then immediately edit: the frame border must keep its padding.
      displayTime = 1400;
      state.editRow = from;
      u8g2.clear();
      display.drawAlarmEdit();
      display.finish();
      displayTime = 1417;
      state.isEditing = true;
      u8g2.clear();
      display.drawAlarmEdit();
      display.verify("quick edit");
      display.finish();
      auto r = display.animation.current(displayTime), target = display.focus;
      if (r.x != target.x || r.y != target.y || r.w != target.w || r.h != target.h) {
        fputs("Editing outline did not settle around its field\n", stderr);
        return 1;
      }
    }
  printf("Actual firmware focus compositor: %u animated frames and 64 quick-edit cases passed\n",
         animatedFrames);
  settleFocus=true;
  for (int row = 0; row < 8; row++)
    for (int hour = 0; hour < 24; hour++)
      for (int minute = 0; minute < 60; minute++) {
        state.editRow = row;
        state.tempAlarm.hour = hour;
        state.tempAlarm.minute = minute;
        state.isEditing = true;
        u8g2.clear();
        display.drawAlarmEdit();
        display.verify("alarm");
        display.finish();
      }
  state.editRow = 1;
  state.tempAlarm.hour = 23;
  state.tempAlarm.minute = 59;
  u8g2.clear();
  display.drawAlarmEdit();
  display.finish();
  save("alarm-edit");
  state.isEditing=false;
  for (int i = 0; i < ALARM_OPTION_ITEMS; i++) {
    state.optionIdx = i;
    state.optionOffset=max(0,i-4);
    u8g2.clear();
    display.drawAlarmOptions();
    display.verify("options");
    display.finish();
  }
  save("alarm-options");
  for (int i = 0; i < 3; i++) {
    state.snoozeIdx = i;
    u8g2.clear();
    display.drawSnooze();
    display.verify("snooze");
    display.finish();
  }
  save("snooze");
  for (int i = 0; i < 2; i++) {
    state.lightIdx = i;
    u8g2.clear();
    display.drawLightLearning();
    display.verify("light");
    display.finish();
  }
  save("light");
  state.isEditing=false;
  for (int i = 0; i < MOTION_ITEMS; i++) {
    state.motionIdx = i;
    state.motionOffset = max(0, i - 4);
    u8g2.clear();
    display.drawMotionSettings();
    display.verify("motion");
    display.finish();
  }
  save("motion");
  for(int i=0;i<INPUT_ITEMS;i++) {
    state.inputIdx=i;state.inputOffset=max(0,i-4);
    u8g2.clear();display.drawInputSettings();display.verify("input");display.finish();
  }
  save("input");
  for(int i=0;i<MAIN_ITEMS;i++) {
    state.menuIdx=i;state.menuOffset=max(0,i-3);
    u8g2.clear();display.drawMainMenu();display.verify("main");display.finish();
    if(i==3)save("main-top");
  }
  save("main-bottom");
  for(bool on:{false,true}) {
    state.abcEnabled=on;state.abcConfirmTargetOn=!on;
    u8g2.clear();display.drawAbcConfirm();display.verify("abc");display.finish();
    save(on?"abc-on":"abc-off");
  }
  u8g2.clear();
  display.drawCo2Setting();
  assert(!display.queued);display.finish();
  save("co2-setting");
  u8g2.clear();
  display.drawDaySelect();
  display.finish();
  save("days");
  newUiChecks();
  consistencyChecks();
  keyboardMotionIntegrity();
  puts(
      "Actual U8g2 raster checks passed: 11,520 alarm field/time combinations, options and frame "
      "padding.");
}
