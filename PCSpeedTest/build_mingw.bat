@echo off
rem ---------------------------------------------------------------
rem  Build PCSpeedTest with the MinGW-w64 (WinLibs) toolchain on its own.
rem  The suite build does this too; this script is for working on the
rem  program without rebuilding everything.
rem  Requires: windres.exe and g++.exe on PATH (or set MINGW_BIN).
rem
rem  Resources are compiled in %TEMP%: gcc's -B override cannot carry a path
rem  with a space in it, and this folder lives under "Zanphar's WinSuite".
rem ---------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "STAGE=%TEMP%\PCSpeedTest-mingw"

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

echo [1/2] Compiling resources (manifest, version info^)...
if exist "%STAGE%" rd /s /q "%STAGE%"
md "%STAGE%\res" || exit /b 1
copy /y "res\*" "%STAGE%\res\" >nul
pushd "%STAGE%"
windres -I res "res\app.rc" -O coff -o default-manifest.o
set "RCERR=%errorlevel%"
popd
if not "%RCERR%"=="0" exit /b 1

echo [2/2] Compiling and linking...
g++ -std=c++17 -O2 -Wall -municode -DUNICODE -D_UNICODE -mwindows -static -s ^
    "-B%STAGE%/" ^
    src\pcspeedtest.cpp src\main.cpp ^
    -o PCSpeedTest.exe ^
    -lwinhttp -liphlpapi -lws2_32 -ladvapi32 -lshell32 -lole32 -luuid ^
    -lgdi32 -luser32 -ldwmapi -ld3d11 -ld3dcompiler
if errorlevel 1 exit /b 1

echo.
echo Build OK: PCSpeedTest.exe
echo   PCSpeedTest.exe --run        measure all fifteen and append a CSV line
echo   PCSpeedTest.exe --selftest   prove the engine without network traffic
endlocal
