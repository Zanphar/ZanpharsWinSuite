@echo off
rem ---------------------------------------------------------------
rem  Build Internet Tuning Wizard with the MinGW-w64 (WinLibs) toolchain.
rem  The suite build does this too; this script is for working on the
rem  program without rebuilding everything.
rem  Requires: windres.exe and g++.exe on PATH (or set MINGW_BIN).
rem
rem  Resources are compiled in %TEMP%: gcc's -B override cannot carry a
rem  path with a space in it, and this folder lives under "Zanphar's WinSuite".
rem ---------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "STAGE=%TEMP%\InternetTuningWizard-mingw"

if not "%MINGW_BIN%"=="" set "PATH=%MINGW_BIN%;%PATH%"

where windres >nul 2>nul
if errorlevel 1 (
    echo ERROR: windres.exe not found on PATH.
    echo        Install MinGW-w64 ^(e.g. via Winget^) or set MINGW_BIN.
    exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
    echo ERROR: g++.exe not found on PATH.
    echo        Install MinGW-w64 ^(e.g. via Winget^) or set MINGW_BIN.
    exit /b 1
)

echo [1/2] Compiling resources (manifest, icon, version info^)...
if exist "%STAGE%" rd /s /q "%STAGE%"
md "%STAGE%\res" || exit /b 1
copy /y "res\*" "%STAGE%\res\" >nul
pushd "%STAGE%"
windres -I res "res\app.rc" -O coff -o default-manifest.o
set "RCERR=%errorlevel%"
popd
if not "%RCERR%"=="0" exit /b 1

echo [2/2] Compiling and linking...
g++ -std=c++17 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -mwindows -static -s ^
    "-B%STAGE%/" ^
    src\tuning.cpp src\main.cpp ^
    -o InternetTuningWizard.exe ^
    -ladvapi32 -liphlpapi -lws2_32 -ldnsapi -lshell32 -luser32 -lgdi32 -ldwmapi
if errorlevel 1 exit /b 1

echo.
echo Build OK: InternetTuningWizard.exe
echo   InternetTuningWizard.exe --selftest   prove the model and the registry engine
echo   InternetTuningWizard.exe --list       every adapter and what this PC reads
endlocal
