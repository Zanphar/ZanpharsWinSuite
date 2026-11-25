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

rem GCC's -B manifest override cannot handle spaces in its path, and windres
rem mangles paths containing an apostrophe. Stage the resources under TEMP so
rem both tools only ever see a clean, space-free path.
set "STAGE=%TEMP%\SystemTurbo-build"
if not "%STAGE%"=="%STAGE: =%" (
    echo ERROR: TEMP contains a space; set TEMP to a space-free folder.
    exit /b 1
)
if exist "%STAGE%" rd /s /q "%STAGE%"
md "%STAGE%\res" || exit /b 1
md "%STAGE%\man" || exit /b 1
copy /y "resources\*" "%STAGE%\res\" >nul || exit /b 1

echo [1/2] Compiling resources (manifest, version info, icon^)...
windres -I "%STAGE%\res" "%STAGE%\res\resources.rc" -O coff -o "%STAGE%\man\default-manifest.o"
if errorlevel 1 exit /b 1

echo [2/2] Compiling and linking...
g++ -std=c++17 -O2 -Wall -municode -DUNICODE -D_UNICODE -mwindows -static -s ^
    -B%STAGE%\man\ ^
    src\app.cpp src\config.cpp src\ram.cpp src\turbo.cpp src\log.cpp src\theme.cpp src\ui.cpp ^
    -o "%STAGE%\SystemTurbo.exe" ^
    -ladvapi32 -lshell32 -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -ldwmapi -lpsapi -lpowrprof
if errorlevel 1 exit /b 1
copy /y "%STAGE%\SystemTurbo.exe" "SystemTurbo.exe" >nul || exit /b 1

echo.
echo Build OK: SystemTurbo.exe
endlocal
