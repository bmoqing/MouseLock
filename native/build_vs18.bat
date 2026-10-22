@echo off
cd /d "%~dp0"

echo Setting up build environment...

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: Failed to initialize Visual Studio environment
    pause
    exit /b 1
)

echo Compiling resources...
rc /nologo /fo resources.res resources.rc
if errorlevel 1 (
    echo ERROR: Resource compilation failed
    pause
    exit /b 1
)

echo Compiling C++ code...
cl /nologo /O2 /utf-8 /EHsc /MT /std:c++17 /DUNICODE /D_UNICODE config.cpp locker.cpp settings.cpp logger.cpp main.cpp /Fe:MouseLock.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:NO resources.res user32.lib shell32.lib advapi32.lib comctl32.lib gdi32.lib psapi.lib
if errorlevel 1 (
    echo ERROR: Compilation failed
    pause
    exit /b 1
)

echo.
echo Build SUCCESS: MouseLock.exe
echo.
pause
exit /b 0
