@echo off
setlocal

rem Zanphar's WinSuite - build script (MinGW-w64 / g++)
rem
rem Uses any g++ already on PATH. If none is found, it falls back to the portable
rem toolchain this project was developed with:
rem   %LOCALAPPDATA%\zanphar-buildtools\w64devkit\bin

where g++ >nul 2>&1
if errorlevel 1 (
    if exist "%LOCALAPPDATA%\zanphar-buildtools\w64devkit\bin\g++.exe" (
        set "PATH=%LOCALAPPDATA%\zanphar-buildtools\w64devkit\bin;%PATH%"
    ) else (
        echo No C++ compiler found.
        echo Install MSYS2/MinGW-w64 and put g++ on PATH, or unpack w64devkit into
        echo   %LOCALAPPDATA%\zanphar-buildtools\
        exit /b 1
    )
)

where windres >nul 2>&1
if errorlevel 1 (
    if exist "%LOCALAPPDATA%\zanphar-buildtools\w64devkit\bin\windres.exe" (
        set "PATH=%LOCALAPPDATA%\zanphar-buildtools\w64devkit\bin;%PATH%"
    ) else (
        echo No resource compiler found. Install MinGW-w64 so windres is on PATH,
        echo or unpack w64devkit into %LOCALAPPDATA%\zanphar-buildtools\
        exit /b 1
    )
)

if not exist build mkdir build
if not exist bin mkdir bin

rem The icon lives in src\app.ico; windres turns src\app.rc into a linkable object so
rem Explorer, the taskbar and the shortcut can read it out of the exe.
echo Compiling resources ...
windres -I src src\app.rc -O coff -o build\app.res.o
if errorlevel 1 (
    echo Resource build failed.
    exit /b 1
)

echo Building ZanpharsWinSuite.exe ...
g++ -std=c++20 -O2 -mwindows -DUNICODE -D_UNICODE ^
    -o bin\ZanpharsWinSuite.exe src\main.cpp src\pages.cpp src\ui.cpp src\core.cpp ^
    src\svcctrl.cpp src\procs.cpp src\browsers.cpp src\dns.cpp src\boost.cpp ^
    src\netgame.cpp src\gameboost.cpp ^
    src\prefs.cpp build\app.res.o ^
    -lgdiplus -ldwmapi -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -lgdi32 -luser32 -lpsapi -lversion -lpowrprof ^
    -lws2_32 -liphlpapi ^
    -static -static-libgcc -static-libstdc++

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Built bin\ZanpharsWinSuite.exe

rem The Task Manager's saved rules need something running when the suite is not: this
rem small helper is put in the per-user Run key the first time a rule is saved.
rem It has no window, so its version block is what Explorer and Task Manager show.
echo Compiling agent resources ...
windres -I src src\agent.rc -O coff -o build\agent.res.o
if errorlevel 1 (
    echo Resource build failed.
    exit /b 1
)

echo Building WinSuiteAgent.exe ...
g++ -std=c++20 -O2 -mwindows -municode -DUNICODE -D_UNICODE ^
    -o bin\WinSuiteAgent.exe src\agent.cpp src\procs.cpp src\core.cpp build\agent.res.o ^
    -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -luser32 -lpsapi -lversion ^
    -static -static-libgcc -static-libstdc++

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Built bin\WinSuiteAgent.exe
echo Running self-test ...
bin\ZanpharsWinSuite.exe --selftest
