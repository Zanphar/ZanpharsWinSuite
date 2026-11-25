@echo off
rem ============================================================================
rem  build_all.bat - build every program in this suite as a static,
rem                  self-contained Win32 executable and put it in bin\.
rem
rem  Usage:  build_all.bat [/clean]
rem            /clean   wipe bin\ and the staging area before building
rem
rem  What it does
rem    1. locates a MinGW-w64 toolchain (MINGW_BIN, PATH, WinGet/WinLibs, ...)
rem       - no MSVC, no vcvars64.bat, no Visual Studio required
rem    2. stages all intermediate files outside the source tree (%TEMP%)
rem    3. per project folder it auto-detects the C++ sources (src\*.cpp), the
rem       resource script (*.rc) and the application manifest, compiles with
rem       -static -municode -mwindows so every bit of the C++ runtime is
rem       linked in, and then copies the .exe into bin\
rem    4. builds the suite launcher with build.bat, which links
rem       bin\ZanpharsWinSuite.exe and runs its --selftest; a failed self-test
rem       fails the run, so a clean build (and release.bat /build) always
rem       leaves a tested launcher in bin\
rem    5. inspects the import table of every output and reports a failure if a
rem       MinGW runtime DLL (libgcc / libstdc++ / libwinpthread) leaked in
rem
rem  The only DLLs an output depends on are the ones Windows itself ships.
rem
rem  Adding a new program: drop its folder next to the others with a src\
rem  folder holding the .cpp files plus (optionally) an .rc and an .app.manifest.
rem  Known projects get a per-project link line below; anything else is built
rem  with the generic library kit and flagged as "auto recipe" in the summary.
rem ============================================================================

setlocal EnableExtensions EnableDelayedExpansion

rem ---------------------------------------------------------------- settings
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || goto :err_root
set "BIN=%ROOT%\bin"

set "CFLAGS=-std=c++17 -O2 -Wall -municode -mwindows -static -s"
set "CFLAGS=%CFLAGS% -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00"

rem Generic link kit used for projects without an explicit entry below.
set "LIBKIT=-lgdiplus -ld2d1 -ldwrite -lcomctl32 -ldwmapi -lpsapi -lpowrprof"
set "LIBKIT=%LIBKIT% -liphlpapi -lws2_32 -ladvapi32 -lshell32 -lole32 -luuid"
set "LIBKIT=%LIBKIT% -lgdi32 -luser32 -lkernel32"

rem Known projects:  folder | output name | link libraries
set "PROJECTS=BrowserCleaner|BrowserCleaner|-lgdiplus -lshell32 -lole32 -luuid -luser32 -lgdi32"
set "PROJECTS=%PROJECTS%;DNSProtect|DNSProtect|-ladvapi32 -lshell32 -lole32 -luuid -liphlpapi -lws2_32 -lcomctl32 -lgdi32 -luser32 -ldwmapi"
set "PROJECTS=%PROJECTS%;SmartRAM|SmartRAM|-ld2d1 -ldwrite -luuid -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32 -lpsapi"
set "PROJECTS=%PROJECTS%;SystemTurbo|SystemTurbo|-ladvapi32 -lshell32 -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -ldwmapi -lpsapi -lpowrprof"
set "PROJECTS=%PROJECTS%;HardWin|HardWin|-ladvapi32 -lshell32 -luser32 -lgdi32 -ldwmapi"
set "PROJECTS=%PROJECTS%;PCSpeedTest|PCSpeedTest|-lwinhttp -liphlpapi -lws2_32 -ladvapi32 -lshell32 -lole32 -luuid -lgdi32 -luser32 -ldwmapi -ld3d11 -ld3dcompiler"
set "PROJECTS=%PROJECTS%;HomePolicy|HomePolicy|-ladvapi32 -lshell32 -lcomdlg32 -luser32 -lgdi32 -ldwmapi"
set "PROJECTS=%PROJECTS%;InternetTuningWizard|InternetTuningWizard|-ladvapi32 -liphlpapi -lws2_32 -ldnsapi -lshell32 -luser32 -lgdi32 -ldwmapi"
set "PROJECTS=%PROJECTS%;InternetConnectionUtility|InternetConnectionUtility|-lgdiplus -lwlanapi -liphlpapi -lwinhttp -ldwmapi -lws2_32 -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32"
set "PROJECTS=%PROJECTS%;"

set "FAILED=0"
set "BUILT=0"
set "AUTOBUILT="
set "DO_CLEAN="
if /i "%~1"=="/clean" set "DO_CLEAN=1"
if /i "%~1"=="-c"    set "DO_CLEAN=1"
if /i "%~1"=="/?"    goto :usage
if /i "%~1"=="-h"    goto :usage
if /i "%~1"=="/help" goto :usage

echo ============================================================================
echo  Zanphar's WinSuite - static MinGW build
echo  root: %ROOT%
echo ============================================================================

rem ------------------------------------------------------------ 1. toolchain
echo.
echo [1/4] Locating the MinGW-w64 toolchain ...
set "TOOL_BIN="
if defined MINGW_BIN if exist "%MINGW_BIN%\g++.exe" if exist "%MINGW_BIN%\windres.exe" set "TOOL_BIN=%MINGW_BIN%"
if not defined TOOL_BIN for /f "delims=" %%I in ('where g++.exe 2^>nul') do if not defined TOOL_BIN if exist "%%~dpIwindres.exe" set "TOOL_BIN=%%~dpI"
if not defined TOOL_BIN for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\*WinLibs*") do if not defined TOOL_BIN if exist "%%~D\mingw64\bin\windres.exe" set "TOOL_BIN=%%~D\mingw64\bin"
if not defined TOOL_BIN for %%D in ("C:\mingw64\bin" "C:\msys64\mingw64\bin" "%ProgramFiles%\mingw64\bin" "%ProgramData%\mingw64\bin" "C:\TDM-GCC-64\bin") do if not defined TOOL_BIN if exist "%%~D\windres.exe" set "TOOL_BIN=%%~D"
if not defined TOOL_BIN goto :err_toolchain
if "%TOOL_BIN:~-1%"=="\" set "TOOL_BIN=%TOOL_BIN:~0,-1%"
for %%T in (g++.exe windres.exe objdump.exe) do if not exist "%TOOL_BIN%\%%T" goto :err_toolchain
set "PATH=%TOOL_BIN%;%PATH%"
for /f "delims=" %%V in ('g++ -dumpversion') do set "GCCVER=%%V"
echo       found: %TOOL_BIN%
echo       g++ %GCCVER%

rem -------------------------------------------------------------- 2. staging
echo.
echo [2/4] Preparing the staging area ...
set "STAGE="
call :pick_stage "%TEMP%\Zanphar-WinSuite-build"
call :pick_stage "%LOCALAPPDATA%\Zanphar-WinSuite-build"
call :pick_stage "%SystemRoot%\Temp\Zanphar-WinSuite-build"
call :pick_stage "C:\Zanphar-WinSuite-build"
if not defined STAGE goto :err_stage
set "PATH=%TOOL_BIN%;%PATH%"

if not defined DO_CLEAN goto :clean_done
echo       /clean: clearing bin\ and the staging area ...
if exist "%BIN%" rd /s /q "%BIN%" 2>nul
if exist "%STAGE%" rd /s /q "%STAGE%" 2>nul
:clean_done

rem gcc's -B manifest override cannot handle spaces in its path, so the compat
rem header (shlobj_core.h is an MSVC SDK header, MinGW only ships shlobj.h) and
rem all object files live in the space-free staging folder.
set "SHIM=%STAGE%\compat"
if not exist "%SHIM%" md "%SHIM%" || goto :err_stage
if not exist "%BIN%" md "%BIN%" || goto :err_stage
if exist "%SHIM%\shlobj_core.h" goto :shim_ready
echo #pragma once> "%SHIM%\shlobj_core.h"
echo #include ^<shlobj.h^>>> "%SHIM%\shlobj_core.h"
echo #include ^<knownfolders.h^>>> "%SHIM%\shlobj_core.h"
:shim_ready
echo       staging: %STAGE%

rem -------------------------------------------------------------- 3. builds
echo.
echo [3/4] Building ...
for %%P in ("%PROJECTS:;=" "%") do for /f "tokens=1,2,* delims=|" %%A in ("%%~P") do call :build_project "%%~A" "%%~B" "%%~C" recipe
rem Projects without a recipe row get the generic library kit. The loop is inlined
rem rather than reached with `call :build_auto`, because a label call placed after
rem the :build_project calls failed with "cannot find the batch label specified -
rem build_auto" on every run, so the fallback never actually ran.
for /d %%D in ("%ROOT%\*") do (
    if exist "%%~D\src\*.cpp" if "!PROJECTS:%%~nxD|=!"=="!PROJECTS!" (
        set "A_DIR=%%~nxD"
        set "A_EXE=!A_DIR!"
        if /i "!A_EXE:~-7!"=="-Modern" set "A_EXE=!A_EXE:~0,-7!"
        if /i "!A_EXE:~-7!"=="-Native" set "A_EXE=!A_EXE:~0,-7!"
        call :build_project "!A_DIR!" "!A_EXE!" "%LIBKIT%" auto
    )
)

rem ------------------------------------------------------- the suite launcher
rem build.bat carries the launcher's own compile line and its self-test, so it is
rem called rather than duplicated here.
echo.
echo   --- ZanpharsWinSuite  -^>  ZanpharsWinSuite.exe
call "%ROOT%\build.bat"
if errorlevel 1 (
    echo       [FAIL] the suite launcher did not build or failed its self-test.
    set /a FAILED+=1
) else (
    echo       [ OK ] bin\ZanpharsWinSuite.exe built, self-test passed
)

rem -------------------------------------------------------------- 4. report
echo.
echo [4/4] Verifying binaries ...
echo.
echo ----------------------------------------------------------------------------
echo  bin\
echo ----------------------------------------------------------------------------
set "TOTAL=0"
set "FOREIGN_TOTAL=0"
for %%X in ("%BIN%\*.exe") do (
    set /a TOTAL+=1
    set "NAME=%%~nxX"
    set /a KB=%%~zX/1024
    set "FOREIGN="
    for /f "tokens=3" %%D in ('objdump -p "%%~fX" 2^>nul ^| findstr /i /c:"DLL Name:"') do (
        echo %%D| findstr /i /c:"libgcc" /c:"libstdc" /c:"libwinpthread" >nul && set "FOREIGN=!FOREIGN! %%D"
    )
    if defined FOREIGN (
        echo   [FAIL] !NAME!  !KB! KB  - depends on!FOREIGN!
        set /a FOREIGN_TOTAL+=1
    ) else (
        echo   [ OK ] !NAME!  !KB! KB  - self-contained, OS DLLs only
    )
)
if "%TOTAL%"=="0" echo   [FAIL] no executables were produced.

echo.
if defined AUTOBUILT echo  Note: built with the generic recipe (check link libs):%AUTOBUILT%
if not "%FAILED%"=="0" goto :done_failed
if not "%FOREIGN_TOTAL%"=="0" goto :done_failed
echo  SUCCESS: %TOTAL% executable^(s^) in %BIN%
endlocal
exit /b 0

:done_failed
echo  FAILED: %FAILED% project^(s^) did not build, %FOREIGN_TOTAL% output^(s^) have foreign DLL dependencies.
endlocal
exit /b 1

rem ============================================================================
rem  :build_project  folder  exename  libs  tag
rem ============================================================================
:build_project
set "P_DIR=%~1"
set "P_EXE=%~2"
set "P_LIBS=%~3"
set "P_TAG=%~4"
set "P_OUT=%STAGE%\%P_DIR%"

echo.
echo   --- %P_DIR%  -^>  %P_EXE%.exe
if not exist "%ROOT%\%P_DIR%\" (
    echo       [FAIL] project folder not found.
    set /a FAILED+=1
    goto :eof
)
pushd "%ROOT%\%P_DIR%"

if not exist "src\*.cpp" (
    echo       [FAIL] no sources found in src\
    popd
    set /a FAILED+=1
    goto :eof
)

rem --- sources (.cpp files, relative paths so quoting stays simple)
set "P_SRC="
for %%C in (src\*.cpp *.cpp) do if exist "%%~C" set "P_SRC=!P_SRC! "%%~C""
if not defined P_SRC (
    echo       [FAIL] no sources found.
    popd
    set /a FAILED+=1
    goto :eof
)

rem --- resource script (first one that exists)
set "P_RC="
for %%R in (src\resources.rc resources\resources.rc res\app.rc src\app.rc app.rc src\*.rc resources\*.rc res\*.rc *.rc) do if not defined P_RC if exist "%%~R" set "P_RC=%%~R"
if defined P_RC for %%R in ("!P_RC!") do set "P_RCDIR=%%~dpR"

rem --- how the manifest gets in: plain resources keep the linker default,
rem     a .manifest file is embedded through a generated override object,
rem     and an .rc that already carries `1 24 "app.manifest"` is used as the
rem     override object itself (the MinGW linker otherwise injects its own).
set "P_MANIFEST="
for %%M in (res\app.manifest resources\app.manifest src\app.manifest app.manifest) do if not defined P_MANIFEST if exist "%%~M" set "P_MANIFEST=%%~M"
set "P_MODE=plain"
if defined P_RC findstr /c:"1 24 " "!P_RC!" >nul 2>nul && set "P_MODE=man-rc"
if "!P_MODE!"=="plain" if defined P_MANIFEST set "P_MODE=man-file"

if exist "!P_OUT!" rd /s /q "!P_OUT!"
md "!P_OUT!" || goto :p_out_failed
set "P_EXTRA="
set "P_BPREFIX="

rem --- resources
rem     windres mangles paths that contain an apostrophe (this suite folder has
rem     one), so the .rc and the files it references are copied into the clean
rem     staging folder first and compiled from there.
if defined P_RC (
    md "!P_OUT!\res" 2>nul
    copy /y "!P_RCDIR!*" "!P_OUT!\res\" >nul
    for %%R in ("!P_RC!") do set "P_RCNAME=%%~nxR"
    pushd "!P_OUT!"
    if "!P_MODE!"=="man-rc" (
        md "man" 2>nul
        echo       [res] !P_RC! -^> manifest override
        windres -I res "res\!P_RCNAME!" -O coff -o "man\default-manifest.o"
    ) else (
        echo       [res] !P_RC!
        windres -I res "res\!P_RCNAME!" -O coff -o "app-res.o"
    )
    set "P_RCERR=!errorlevel!"
    popd
    if not "!P_RCERR!"=="0" goto :p_build_failed
    if not "!P_MODE!"=="man-rc" set "P_EXTRA=!P_EXTRA! "!P_OUT!\app-res.o""
)

rem --- standalone manifest -> override object built from a tiny generated .rc
if "!P_MODE!"=="man-file" (
    md "!P_OUT!\man" 2>nul
    for %%M in ("!P_MANIFEST!") do set "P_MANNAME=%%~nxM"
    > "!P_OUT!\man\manifest.rc" echo 1 24 "!P_MANNAME!"
    echo       [man] !P_MANIFEST!
    pushd "!P_OUT!"
    windres -I res "man\manifest.rc" -O coff -o "man\default-manifest.o"
    set "P_RCERR=!errorlevel!"
    popd
    if not "!P_RCERR!"=="0" goto :p_build_failed
)
if exist "!P_OUT!\man\default-manifest.o" set "P_BPREFIX=-B!P_OUT!\man\"

rem --- compile + link in one step
echo       [cc] !P_EXE!.exe
g++ !CFLAGS! -I"!SHIM!" !P_SRC! !P_EXTRA! -o "!P_OUT!\!P_EXE!.exe" !P_BPREFIX! !P_LIBS!
if errorlevel 1 goto :p_build_failed
if not exist "!P_OUT!\!P_EXE!.exe" goto :p_build_failed

copy /y "!P_OUT!\!P_EXE!.exe" "%BIN%\!P_EXE!.exe" >nul
if errorlevel 1 (
    echo       [FAIL] could not copy to bin\
    popd
    set /a FAILED+=1
    goto :eof
)
if "!P_TAG!"=="auto" set "AUTOBUILT=!AUTOBUILT! !P_DIR!"
set /a BUILT+=1
echo       [ OK ] bin\!P_EXE!.exe
popd
goto :eof

:p_out_failed
echo       [FAIL] could not create the staging folder.
popd
set /a FAILED+=1
goto :eof

:p_build_failed
echo       [FAIL] compilation or linking failed.
popd
set /a FAILED+=1
goto :eof

rem ============================================================================
rem  :pick_stage  candidate-dir - accept the first writable, space-free path
rem ============================================================================
:pick_stage
if defined STAGE goto :eof
set "CAND=%~1"
if not "%CAND%"=="%CAND: =%" goto :eof
if not exist "%CAND%" md "%CAND%" 2>nul
if not exist "%CAND%\" goto :eof
set "STAGE=%CAND%"
goto :eof

rem ============================================================================
rem  errors / help
rem ============================================================================
:err_root
echo ERROR: could not switch to "%~dp0"
exit /b 1

:err_toolchain
echo.
echo ERROR: no MinGW-w64 toolchain found (need g++.exe, windres.exe, objdump.exe).
echo        Install one with:  winget install BrechtSanders.WinLibs.POSIX.UCRT
echo        or point MINGW_BIN at the folder that contains g++.exe, e.g.:
echo           set "MINGW_BIN=C:\mingw64\bin"
endlocal
exit /b 1

:err_stage
echo.
echo ERROR: could not create a writable staging folder without spaces in its
echo        path (tried TEMP, LOCALAPPDATA, Windows\Temp and C:\).
endlocal
exit /b 1

:usage
echo Usage: build_all.bat [/clean]
echo   Builds every project in this folder as a static, self-contained
echo   Win32 executable and copies the results into bin\. The suite launcher
echo   is built by build.bat afterwards and has to pass its --selftest.
echo.
echo   /clean   remove bin\ and the staging area first
endlocal
exit /b 0
