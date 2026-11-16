@echo off
rem ============================================================================
rem  release.bat - package bin\ into a versioned release archive.
rem
rem  Usage:  release.bat [/build] [/version X.Y.Z] [/help]
rem
rem            /build           run build_all.bat /clean first, so the archive
rem                             can only ever contain binaries built just now
rem            /version X.Y.Z   override the version recorded in VERSION
rem
rem  What it does
rem    1. reads the version from the VERSION file (or /version)
rem    2. with /build: wipes bin\ and the staging area and rebuilds every
rem       program; a failed build aborts here and packages nothing
rem    3. stages bin\*.exe plus README.md, CHANGES.md and LICENSE.md into
rem       release\Zanphar-WinSuite-v<version>\
rem    4. writes MANIFEST.txt - every program with its product name, version,
rem       size, description, publisher and SHA-256
rem    5. writes SHA256SUMS.txt - one SHA-256 per packaged file
rem    6. zips the package and writes <archive>.zip.sha256 for the archive
rem
rem  Existing artefacts for the same version are replaced; packages of other
rem  versions in release\ are left untouched.
rem
rem  Without /build this packages whatever is already in bin\ and compiles
rem  nothing. Exit code 0 on success, 1 on failure.
rem ============================================================================

setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || goto :err_root

set "VER="
set "BUILD="
set "ARG=%~1"
if not defined ARG goto :after_args
:parse_args
if /i "%ARG%"=="/?"     goto :usage
if /i "%ARG%"=="-h"     goto :usage
if /i "%ARG%"=="/help"  goto :usage
if /i "%ARG%"=="--help" goto :usage
if /i "%ARG%"=="/build"  goto :arg_build
if /i "%ARG%"=="--build" goto :arg_build
if /i "%ARG%"=="-b"      goto :arg_build
if /i "%ARG%"=="/version"  goto :arg_version
if /i "%ARG%"=="--version" goto :arg_version
if /i "%ARG%"=="/v"        goto :arg_version
echo ERROR: unknown argument "%ARG%"
echo.
goto :usage_err

:arg_build
set "BUILD=1"
shift
if not "%~1"=="" set "ARG=%~1" & goto :parse_args
goto :after_args

:arg_version
shift
if "%~1"=="" (
    echo ERROR: /version needs a value, e.g. /version 0.1.0
    endlocal
    exit /b 1
)
set "VER=%~1"
shift
if not "%~1"=="" set "ARG=%~1" & goto :parse_args
goto :after_args

:after_args
if defined VER goto :have_version

set "VFILE=%ROOT%\VERSION"
if not exist "%VFILE%" goto :err_noversion
for /f "usebackq delims=" %%V in ("%VFILE%") do if not defined VER set "VER=%%V"
if not defined VER goto :err_noversion
rem trim stray spaces from the VERSION file
for /f "tokens=* delims= " %%V in ("%VER%") do set "VER=%%V"
:have_version

echo %VER%| findstr /r /c:"^[0-9][0-9A-Za-z.\-]*$" >nul || goto :err_badversion

echo ============================================================================
echo  Zanphar's WinSuite - release packaging
echo  root:    %ROOT%
echo  version: %VER%
echo ============================================================================
echo.

where powershell.exe >nul 2>nul || goto :err_ps
if not exist "%ROOT%\tools\make_release.ps1" goto :err_script

if not defined BUILD goto :no_build
echo [1/3] Clean rebuild: build_all.bat /clean
echo.
call "%ROOT%\build_all.bat" /clean
if errorlevel 1 goto :err_build
echo.
echo [2/3] Packaging bin\ into release\Zanphar-WinSuite-v%VER%\ ...
goto :stage

:no_build
echo [1/2] Packaging bin\ into release\Zanphar-WinSuite-v%VER%\ ...

:stage
if not exist "%ROOT%\bin\" goto :err_nobin
dir /b "%ROOT%\bin\*.exe" >nul 2>nul || goto :err_nobin

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%ROOT%\tools\make_release.ps1" -Root "%ROOT%" -Version "%VER%"
if errorlevel 1 goto :err_release

echo.
if defined BUILD echo [3/3] Done.
if not defined BUILD echo [2/2] Done.
echo.
echo  SUCCESS: release\Zanphar-WinSuite-v%VER%.zip
if defined BUILD echo  Built from a clean rebuild of every project.
echo  Verify the archive with:  sha256sum -c release\Zanphar-WinSuite-v%VER%.zip.sha256
echo  Verify the contents with: sha256sum -c SHA256SUMS.txt   (inside the package)
endlocal
exit /b 0

rem ============================================================================
rem  errors / help
rem ============================================================================
:err_root
echo ERROR: could not switch to "%ROOT%"
endlocal
exit /b 1

:err_build
echo.
echo  FAILED: the clean rebuild did not succeed - nothing was packaged.
echo  No archive was written. Run build_all.bat on its own to see the full
echo  compiler log, fix the build, then run release.bat /build again.
endlocal
exit /b 1

:err_nobin
echo ERROR: no executables in "%ROOT%\bin".
echo        Run build_all.bat first, then release.bat.
endlocal
exit /b 1

:err_noversion
echo ERROR: no version available - create a VERSION file with the release
echo        version in it, or pass /version X.Y.Z
endlocal
exit /b 1

:err_badversion
echo ERROR: "%VER%" is not a valid version (expected something like 0.1.0)
endlocal
exit /b 1

:err_ps
echo ERROR: powershell.exe was not found on PATH - it is needed for hashing
echo        and for creating the archive.
endlocal
exit /b 1

:err_script
echo ERROR: tools\make_release.ps1 is missing.
endlocal
exit /b 1

:err_release
echo.
echo  FAILED: the release package was not completed - see the error above.
endlocal
exit /b 1

:usage
echo Usage: release.bat [/build] [/version X.Y.Z]
echo   Packages bin\ into release\Zanphar-WinSuite-v^<version^>.zip with
echo   MANIFEST.txt and SHA256SUMS.txt.
echo.
echo   /build           run build_all.bat /clean first, then package - the
echo                    archive can then only contain freshly built binaries
echo   /version X.Y.Z   override the version stored in the VERSION file
endlocal
exit /b 0

:usage_err
echo Usage: release.bat [/build] [/version X.Y.Z]
endlocal
exit /b 1
