@echo off
rem ---------------------------------------------------------------
rem  Build System Turbo with the MinGW-w64 (WinLibs) toolchain.
rem  Requires: windres.exe and g++.exe on PATH (or set MINGW_BIN).
rem ---------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "BASE=%~dp0"

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

echo [1/2] Compiling resources (manifest, version info, icon^)...
if not exist mingw-override md mingw-override
windres -I resources resources\resources.rc -O coff -o mingw-override\default-manifest.o
if errorlevel 1 exit /b 1

echo [2/2] Compiling and linking...
g++ -std=c++17 -O2 -Wall -municode -DUNICODE -D_UNICODE -mwindows -static -s ^
    "-B%BASE%mingw-override/" ^
    src\app.cpp src\config.cpp src\ram.cpp src\turbo.cpp src\log.cpp src\theme.cpp src\ui.cpp ^
    -o SystemTurbo.exe ^
    -ladvapi32 -lshell32 -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -ldwmapi -lpsapi -lpowrprof
if errorlevel 1 exit /b 1

echo.
echo Build OK: SystemTurbo.exe
endlocal
