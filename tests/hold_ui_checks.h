// Exercise the real reset, setup and WiFi input branches with the real OLED renderer.
static void holdFrame(uint32_t now) {
  displayTime=now;
  cleanCanvas();
  if(state.uiState==RESET_CONFIRM)display.drawResetConfirm();
  else if(state.uiState==INPUT_SETTING)display.drawInputSettings();
  else if(state.uiState==MOTION_SETTING)display.drawMotionSettings();
  else display.drawWiFiList();
  display.verify("hold target padding");
  display.finish();
}
static void holdUiChecks() {
  auto savedState=state;
  auto savedInput=inputSettings;
  auto savedMotion=motionSettings;
  auto savedAp=apMgr;
  bool savedSettle=settleFocus;
  settleFocus=true;
  unsigned resetCases=0,configuredCases=0;
  assert(oled::InputSettings{}.holdDelayMs==1000);
  assert(FULL_RESET_HOLD_MS==3000);
  for(uint32_t start : {10000U,0xfffffff0U})
    for(int delay=300;delay<=3000;delay+=50) {
      // Reset remains 3 seconds for every general hold setting, including rollover.
      inputSettings.holdDelayMs=delay;
      inputSettings.repeatDelayMs=delay%100?200:1000;
      state.uiState=RESET_CONFIRM;
      state.isEditing=false;
      state.resetConfirmStep=0;
      state.resetConfirmYes=true;
      int before=fullResetCalls;
      for(int step=0;step<2;step++) {
        uint32_t began=start+step*7000;
        if(step==1) {
          assert(!state.resetConfirmYes && state.resetConfirmStep==1);
          setupInput(false,false,false,false,began-2);
          setupInput(false,true,false,false,began-1);
          assert(state.resetConfirmYes);
        }
        setupInput(false,false,true,true,began);
        for(int ms : {0,1500,2999,3000}) {
          setupInput(false,false,false,true,began+ms);
          assert(fullResetCalls==before && state.resetConfirmStep==step);
          holdFrame(began+ms);
          assert(uiHold.running());
          assert(uiHold.fraction(began+ms)==(ms==3000?1.f:float(ms)/3000));
        }
        setupInput(false,false,false,true,began+3001);
        assert(state.resetConfirmStep==1 && !uiHold.running());
        assert(fullResetCalls==before+(step==1));
        if(step==0)assert(!state.resetConfirmYes);
        else assert(fullResetAt==uint32_t(began+3001));
        setupInput(false,false,false,true,began+6000);
        assert(fullResetCalls==before+(step==1) && !uiHold.running());
        resetCases++;
      }
      // All configurable holds use the selected duration, not the previous constants.
      for(UIState page : {INPUT_SETTING,MOTION_SETTING,WIFI_LIST}) {
        cancelMenuHold();
        inputSettings.holdDelayMs=delay;
        inputSettings.repeatDelayMs=delay%100?200:1000;
        inputSettings.debounceMs=50;
        motionSettings.jerk=25;
        state.uiState=page;
        state.inputIdx=INPUT_ITEMS-1;state.inputOffset=INPUT_ITEMS-5;
        state.motionIdx=MOTION_ITEMS-1;state.motionOffset=3;
        state.listIdx=state.listOffset=0;
        apMgr.apList={{"Saved AP","",-55,true,delay%100?1:3,false}};
        apMgr.connectStatus=0;
        int oldInputSaves=inputSaves,oldMotionSaves=motionSaves,oldDeletes=wifiStorage.removed;
        setupInput(false,false,true,true,start);
        assert(uiHold.running());
        holdFrame(start+delay/2);
        assert(uiHold.fraction(start+delay/2)==.5f);
        holdFrame(start+delay-1);
        setupInput(false,false,false,true,start+delay);
        assert(inputSaves==oldInputSaves && motionSaves==oldMotionSaves && wifiStorage.removed==oldDeletes);
        holdFrame(start+delay);
        setupInput(false,false,false,true,start+delay+1);
        assert(!uiHold.running());
        if(page==INPUT_SETTING) {
          assert(inputSettings.holdDelayMs==1000 && inputSettings.debounceMs==5);
          assert(inputSettings.repeatDelayMs==600);
          assert(inputSaves==oldInputSaves+1 && motionSettings.jerk==25);
        } else if(page==MOTION_SETTING) {
          assert(motionSettings.jerk==60 && motionSaves==oldMotionSaves+1);
          assert(inputSettings.holdDelayMs==delay && inputSettings.debounceMs==50);
          assert(inputSettings.repeatDelayMs==(delay%100?200:1000));
        } else assert(wifiStorage.removed==oldDeletes+1 && state.uiState==WIFI_SCANNING);
        setupInput(false,false,false,true,start+delay+100);
        assert(inputSaves==oldInputSaves+(page==INPUT_SETTING));
        assert(motionSaves==oldMotionSaves+(page==MOTION_SETTING));
        assert(wifiStorage.removed==oldDeletes+(page==WIFI_LIST));
        configuredCases++;
      }
    }
  // Releasing Yes cancels, even after the fully filled frame, and never accumulates taps.
  int before=fullResetCalls;
  for(int step=0;step<2;step++) {
  state.uiState=RESET_CONFIRM;state.resetConfirmYes=true;state.resetConfirmStep=step;
  for(int release : {1,2999,3000}) {
    setupInput(false,false,true,true,10000);
    holdFrame(10000+release);
    setupInput(false,false,false,false,10000+release);
    assert(!uiHold.running() && fullResetCalls==before);
    assert(state.resetConfirmStep==step);
  }
  setupInput(false,false,true,true,20000);
  holdFrame(22999);
  setupInput(true,false,false,true,23000);
  assert(!state.resetConfirmYes && !uiHold.running() && fullResetCalls==before);
  setupInput(false,true,false,true,23001);
  assert(state.resetConfirmYes && !uiHold.running());
  setupInput(false,false,false,true,27000);
  assert(fullResetCalls==before); // Moving back to Yes with OK held cannot rearm.
  state.resetConfirmYes=false;
  setupInput(false,false,true,true,28000);
  assert(state.uiState==MAIN_MENU && fullResetCalls==before);
  }

  // Independent pixel oracle: no inset moat, and only the part to the left fills.
  for(int step=0;step<2;step++) {
  state.uiState=RESET_CONFIRM;state.resetConfirmYes=true;state.resetConfirmStep=step;
  setupInput(false,false,true,true,30000);
  for(int ms : {0,1500,3000}) {
    displayTime=30000+ms;
    cleanCanvas();display.drawResetConfirm();display.verify("reset yes glyph");
    uint8_t raw[1024];memcpy(raw,u8g2.bytes,1024);
    auto r=display.focus;
    int width=ms==0?0:ms==1500?(r.w-2)/2:r.w-2;
    display.finish();
    for(int y=0;y<64;y++)for(int x=0;x<128;x++) {
      bool expected=raw[(y>>3)*128+x]&(1<<(y&7));
      bool in=x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;
      bool edge=in && (x==r.x || x==r.x+r.w-1 || y==r.y || y==r.y+r.h-1);
      if(edge)expected=true;
      else if(in && x<r.x+1+width)expected=!expected;
      assert(u8g2.lit(x,y)==expected);
    }
    char frameName[48];
    snprintf(frameName,sizeof(frameName),"reset-step%d-%s",step+1,ms==0?"empty":ms==1500?"half":"full");
    save(frameName);
  }
  cancelMenuHold();
  }
  // Real setting edits preserve all 55 choices, and both bounds.
  state.uiState=INPUT_SETTING;state.inputIdx=2;state.inputOffset=0;state.isEditing=true;
  inputSettings.holdDelayMs=1000;
  for(int i=0;i<80;i++)setupInput(true,false,false,false,40000+i);
  assert(inputSettings.holdDelayMs==300);
  for(int delay=300;delay<=3000;delay+=50) {
    assert(inputSettings.holdDelayMs==delay);
    setupInput(false,true,false,false,42000+delay);
  }
  assert(inputSettings.holdDelayMs==3000);
  holdFrame(48000);save("hold-delay-max");
  cancelMenuHold();
  state=savedState;inputSettings=savedInput;motionSettings=savedMotion;apMgr=savedAp;
  settleFocus=savedSettle;
  inputSaves=motionSaves=wifiStorage.removed=fullResetCalls=0;
  display.animation=oled::UiAnimation{};display.scrollView=-1;display.rolling.reset();
  printf("Hold UI: %u fixed-reset cases, %u configured setup/WiFi holds, release/navigation cancellation, 55 settings and exact fill pixels passed.\n",resetCases,configuredCases);
}
