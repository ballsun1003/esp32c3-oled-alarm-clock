const fs=require('node:fs'),path=require('node:path');
const root=path.join(__dirname,'..'),s=fs.readFileSync(path.join(root,'co2.ino'),'utf8');
const start=s.indexOf('class AlarmManager {'),end=s.indexOf('} alarmMgr;',start)+11;
if(start<0||end<start)throw Error('Alarm manager missing');
fs.writeFileSync(path.join(root,'_backup/alarm-manager.h'),s.slice(start,end));
fs.writeFileSync(path.join(root,'_backup/alarm-options-test.h'),s.match(/struct AlarmOptions \{[\s\S]*?\n\};/)[0]);
fs.writeFileSync(path.join(root,'_backup/input-save.h'),s.match(/void saveInputSettings\(\) \{[\s\S]*?\n\}/)[0]);
