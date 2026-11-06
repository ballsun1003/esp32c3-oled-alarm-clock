@echo off
if not exist _backup mkdir _backup
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /EHsc /O2 /Fe:_backup\native-motion.exe /Fo:_backup\native-motion.obj tests\native_motion.cpp
if errorlevel 1 exit /b %errorlevel%
_backup\native-motion.exe
