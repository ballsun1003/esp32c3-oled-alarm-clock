static void repeatDelayChecks() {
  auto savedState=state;
  auto savedInput=inputSettings;
  bool savedSettle=settleFocus;
  settleFocus=true;
  unsigned keyboardCases=0;
  // A sampled direction reversal is a new press, even if no all-up sample occurs.
  for(uint32_t start : {10000U,0xfffffff0U})
    for(int delay : {200,600,1000}) {
      inputSettings={};inputSettings.repeatDelayMs=delay;
      textInputMgr.reset();btnLeft.pressed=false;btnRight.pressed=true;
      displayTime=start;textInputMgr.processInput(false);
      displayTime=start+61;textInputMgr.processInput(false); // b
      displayTime=start+62+delay;textInputMgr.processInput(false); // c, accelerated
      btnLeft.pressed=true;btnRight.pressed=false;
      displayTime++;textInputMgr.processInput(false);
      displayTime+=61;textInputMgr.processInput(false); // b, first left step
      uint32_t leftStep=displayTime;
      displayTime=leftStep+101;textInputMgr.processInput(true);
      assert(textInputMgr.getInputString()=="b"); // No inherited 100ms repeat interval.
      displayTime=leftStep+delay+1;textInputMgr.processInput(true);
      assert(textInputMgr.getInputString()=="ba");
      btnLeft.pressed=false;textInputMgr.processInput(false);
    }
  for(uint32_t start : {10000U,0xfffffff0U})
    for(int hold : {300,1000,3000})
      for(int delay=200;delay<=1000;delay+=50) {
        inputSettings.holdDelayMs=hold;
        inputSettings.repeatDelayMs=delay;
        inputSettings.chordMs=60;
        inputSettings.repeatHz=10;
        textInputMgr.reset();
        btnLeft.pressed=false;btnRight.pressed=true;
        displayTime=start;textInputMgr.processInput(false);
        displayTime=start+61;textInputMgr.processInput(false); // First step after chord grace.
        displayTime=start+61+delay;textInputMgr.processInput(true);
        assert(textInputMgr.getInputString()=="b"); // No repeat at the strict boundary.
        displayTime++;textInputMgr.processInput(true);
        assert(textInputMgr.getInputString()=="bc");
        displayTime+=100;textInputMgr.processInput(false);
        displayTime++;textInputMgr.processInput(true);
        assert(textInputMgr.getInputString()=="bcd");
        btnRight.pressed=false;textInputMgr.processInput(false);
        keyboardCases++;
      }
  state.uiState=INPUT_SETTING;state.inputIdx=3;state.inputOffset=0;state.isEditing=false;
  inputSettings={};inputSettings.holdDelayMs=2350;
  setupInput(false,false,true,true,10000);
  assert(state.isEditing);
  for(int i=0;i<30;i++)setupInput(true,false,false,false,11000+i);
  assert(inputSettings.repeatDelayMs==200);
  for(int delay=200;delay<=1000;delay+=50) {
    assert(inputSettings.repeatDelayMs==delay && inputSettings.holdDelayMs==2350);
    displayTime=12000+delay;display.rolling.reset();
    cleanCanvas();display.drawInputSettings();display.verify("repeat wait padding");
    uint8_t actual[1024];memcpy(actual,u8g2.bytes,1024);
    cleanCanvas();u8g2.setFont(u8g2_font_6x10_tr);
    char label[32];snprintf(label,sizeof(label),"Repeat Wait [%4dms]",delay);
    u8g2.drawStr(4,48,label);
    sameRegion(actual,{2,41,124,9},"repeat wait independent glyph oracle");
    memcpy(u8g2.bytes,actual,1024);display.finish();
    setupInput(false,true,false,false,14000+delay);
  }
  assert(inputSettings.repeatDelayMs==1000 && inputSettings.repeatHz==10 && inputSettings.chordMs==60);
  setupInput(false,false,true,true,16000);
  assert(!state.isEditing);
  // The existing rate/chord edits and Defaults remain on the correct shifted rows.
  setupInput(false,true,false,false,16010);assert(state.inputIdx==4);
  setupInput(false,false,true,true,16020);
  setupInput(true,false,false,false,16030);assert(inputSettings.repeatHz==9);
  setupInput(false,false,true,true,16040);
  setupInput(false,true,false,false,16050);assert(state.inputIdx==5 && state.inputOffset==1);
  setupInput(false,false,true,true,16060);
  setupInput(false,true,false,false,16070);assert(inputSettings.chordMs==70);
  setupInput(false,false,true,true,16080);
  setupInput(false,true,false,false,16090);assert(state.inputIdx==6 && state.inputOffset==2);
  holdFrame(17000);save("split-input-bottom");
  state.inputIdx=3;state.inputOffset=0;inputSettings={};
  display.rolling.reset();holdFrame(18000);save("split-input-default");
  state=savedState;inputSettings=savedInput;settleFocus=savedSettle;
  inputSaves=0;
  textInputMgr.reset();display.animation=oled::UiAnimation{};display.scrollView=-1;display.rolling.reset();
  printf("Repeat wait UI: %u keyboard hold/wait/rollover combinations, 17 independent menu values, shifted rows and glyph bounds passed.\n",keyboardCases);
}
