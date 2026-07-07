@echo off
setlocal enabledelayedexpansion
chcp 65001 >NUL
REM ============================================================
REM  Regenerate Visual Studio project files (.sln / .vcxproj)
REM  When to use: after adding/removing .h .cpp files so VS sees them.
REM  Note: goes through engine Build.bat which uses the bundled .NET,
REM        do NOT call UnrealBuildTool.exe directly (wrong .NET version).
REM ============================================================

REM ---- UE_ROOT: read from local config file (ue_root.cfg) ----
set "CONFIG_FILE=%~dp0ue_root.cfg"
if exist "%CONFIG_FILE%" (
    set /p UE_ROOT=<"%CONFIG_FILE%"
) else (
    echo.
    echo [INFO] UE engine root not configured.
    echo Please enter your UE 5.8 installation path
    echo   Example: D:\Epic Games\UE_5.8
    echo.
    set /p UE_ROOT="UE_ROOT path: "
    if not defined UE_ROOT (
        echo [ERROR] No path entered. Exiting.
        pause
        exit /b 1
    )
    echo !UE_ROOT!>"%CONFIG_FILE%"
    echo [OK] Saved to %CONFIG_FILE%
    echo.
)

set "PROJECT=%~dp0LetsDeepRock.uproject"
set "BUILD_BAT=!UE_ROOT!\Engine\Build\BatchFiles\Build.bat"

echo ============================================================
echo  Regenerating project files
echo  Project: %PROJECT%
echo ============================================================

if not exist "!BUILD_BAT!" (
    echo [ERROR] Engine Build.bat not found: "!BUILD_BAT!"
    echo Check ue_root.cfg or delete it to re-enter the path.
    pause
    exit /b 1
)

if not exist "%PROJECT%" (
    echo [ERROR] Project file not found: "%PROJECT%"
    pause
    exit /b 1
)

call "!BUILD_BAT!" -projectfiles -project="%PROJECT%" -game -engine -progress

if errorlevel 1 (
    echo [FAILED] Failed to generate project files.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  [SUCCESS] Project files generated.
echo ============================================================
pause
