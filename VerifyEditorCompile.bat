@echo off
setlocal

set "CONFIG_FILE=%~dp0ue_root.cfg"
set "PROJECT=%~dp0LetsDeepRock.uproject"

if not exist "%CONFIG_FILE%" (
    echo [ERROR] Missing UE root config: "%CONFIG_FILE%"
    exit /b 1
)

set /p UE_ROOT=<"%CONFIG_FILE%"
if not defined UE_ROOT (
    echo [ERROR] UE root config is empty: "%CONFIG_FILE%"
    exit /b 1
)

set "BUILD_BAT=%UE_ROOT%\Engine\Build\BatchFiles\Build.bat"

if not exist "%BUILD_BAT%" (
    echo [ERROR] Unreal Build.bat not found: "%BUILD_BAT%"
    exit /b 1
)

if not exist "%PROJECT%" (
    echo [ERROR] Project file not found: "%PROJECT%"
    exit /b 1
)

echo [INFO] Verifying LetsDeepRockEditor compile...
call "%BUILD_BAT%" LetsDeepRockEditor Win64 Development -Project="%PROJECT%" -WaitMutex -NoHotReloadFromIDE -ActionTypeFilter=Compile
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
    echo [FAILED] Compile verification failed with exit code %EXIT_CODE%.
    exit /b %EXIT_CODE%
)

echo [SUCCESS] Compile verification passed.
exit /b 0
