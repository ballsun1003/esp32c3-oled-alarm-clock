@echo off
if not exist _backup mkdir _backup
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
node tools\prepare_display_test.cjs
if errorlevel 1 exit /b %errorlevel%
cl @_backup/display.rsp
if errorlevel 1 exit /b %errorlevel%
cl @_backup/display-link.rsp
if errorlevel 1 exit /b %errorlevel%
_backup\display-test.exe
