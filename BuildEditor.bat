@echo off
chcp 65001 >NUL
REM ============================================================
REM  LetsDeepRock editor incremental build
REM  Use after changing C++ code (fast, incremental).
REM  Usage: double-click, or run BuildEditor.bat from cmd.
REM ============================================================

set "UE_ROOT=D:\Epic Games\UE_5.8"
set "PROJECT=%~dp0LetsDeepRock.uproject"
set "TARGET=LetsDeepRockEditor"
set "PLATFORM=Win64"
set "CONFIG=Development"

set "BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"

echo ============================================================
echo  Build target: %TARGET% %PLATFORM% %CONFIG%
echo  Project: %PROJECT%
echo ============================================================

if not exist "%BUILD_BAT%" (
    echo [ERROR] Engine Build.bat not found: "%BUILD_BAT%"
    echo Check UE_ROOT.
    pause
    exit /b 1
)

if not exist "%PROJECT%" (
    echo [ERROR] Project file not found: "%PROJECT%"
    pause
    exit /b 1
)

REM ---- Warn if editor is running (dll may be locked) ----
tasklist /FI "IMAGENAME eq UnrealEditor.exe" 2>NUL | find /I "UnrealEditor.exe" >NUL
if not errorlevel 1 (
    echo.
    echo [WARN] UnrealEditor is running.
    echo   - Live Coding: you may continue (Ctrl+Alt+F11 is easier)
    echo   - If you added/removed files: close the editor first, or build may fail.
    echo.
    pause
)

call "%BUILD_BAT%" %TARGET% %PLATFORM% %CONFIG% -Project="%PROJECT%" -WaitMutex -FromMsBuild

if errorlevel 1 (
    echo.
    echo ============================================================
    echo  [FAILED] Build error, see log above.
    echo ============================================================
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  [SUCCESS] Build finished.
echo ============================================================
pause
