@echo off
if not exist _backup mkdir _backup
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
node tools\prepare_alarm_test.cjs
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /Fe:_backup/alarm-test.exe /Fo:_backup/alarm-test.obj tests/native_alarm.cpp
if errorlevel 1 exit /b %errorlevel%
_backup\alarm-test.exe
