@echo off
rem Build the Browser Cleaner clone with MinGW-w64 g++
cd /d "%~dp0"

rem 1. compile resources (icon + version info)
windres src\resources.rc -O coff -o src\resources.o
if %errorlevel% neq 0 (
  echo Resources FAILED.
  exit /b 1
)

rem 2. compile + link (statically linked, no external DLL dependencies)
g++ -std=c++17 -O2 -Wall -municode -mwindows src\main.cpp src\resources.o ^
    -o BrowserCleaner.exe ^
    -lgdiplus -lshell32 -lole32 -luuid -luser32 -lgdi32 ^
    -static-libgcc -static-libstdc++
if %errorlevel% neq 0 (
  echo Build FAILED.
  exit /b 1
)
echo Build OK: BrowserCleaner.exe
