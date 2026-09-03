@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :err

rc /nologo /fo resources.res resources.rc
if errorlevel 1 goto :err

cl /nologo /O2 /utf-8 /EHsc /MT /std:c++17 /DUNICODE /D_UNICODE ^
  config.cpp locker.cpp settings.cpp main.cpp ^
  /Fe:MouseLock.exe ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO resources.res ^
  user32.lib shell32.lib advapi32.lib comctl32.lib gdi32.lib
if errorlevel 1 goto :err

echo Build OK: MouseLock.exe
exit /b 0

:err
echo Build FAILED
exit /b 1