@echo off
chcp 65001 >NUL
REM ============================================================
REM  Regenerate Visual Studio project files (.sln / .vcxproj)
REM  When to use: after adding/removing .h .cpp files so VS sees them.
REM  Note: goes through engine Build.bat which uses the bundled .NET,
REM        do NOT call UnrealBuildTool.exe directly (wrong .NET version).
REM ============================================================

set "UE_ROOT=D:\Epic Games\UE_5.8"
set "PROJECT=%~dp0LetsDeepRock.uproject"
set "BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"

echo ============================================================
echo  Regenerating project files
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

call "%BUILD_BAT%" -projectfiles -project="%PROJECT%" -game -engine -progress

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
