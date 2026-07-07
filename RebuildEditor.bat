@echo off
chcp 65001 >NUL
REM ============================================================
REM  LetsDeepRock editor clean rebuild
REM  When to use: after editing .Build.cs / adding-removing files /
REM               when incremental build gives strange link errors.
REM  Slower than BuildEditor.bat but cleanest.
REM ============================================================

set "UE_ROOT=D:\Epic Games\UE_5.8"
set "PROJECT=%~dp0LetsDeepRock.uproject"
set "TARGET=LetsDeepRockEditor"
set "PLATFORM=Win64"
set "CONFIG=Development"

set "BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"

echo ============================================================
echo  Clean rebuild: %TARGET% %PLATFORM% %CONFIG%
echo ============================================================

if not exist "%BUILD_BAT%" (
    echo [ERROR] Engine Build.bat not found: "%BUILD_BAT%"
    pause
    exit /b 1
)

REM ---- Must close editor before clean rebuild ----
tasklist /FI "IMAGENAME eq UnrealEditor.exe" 2>NUL | find /I "UnrealEditor.exe" >NUL
if not errorlevel 1 (
    echo [ERROR] UnrealEditor is running. Close it before clean rebuild.
    pause
    exit /b 1
)

echo [1/2] Clean...
call "%BUILD_BAT%" %TARGET% %PLATFORM% %CONFIG% -Project="%PROJECT%" -Clean
if errorlevel 1 (
    echo [FAILED] Clean stage error.
    pause
    exit /b 1
)

echo [2/2] Build...
call "%BUILD_BAT%" %TARGET% %PLATFORM% %CONFIG% -Project="%PROJECT%" -WaitMutex -FromMsBuild
if errorlevel 1 (
    echo [FAILED] Build stage error.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  [SUCCESS] Clean rebuild finished.
echo ============================================================
pause
