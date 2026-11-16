# tools/build.ps1 — one-shot MSVC build (Release, statically-linked CRT).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }

$genNinja = (Get-Command ninja -ErrorAction SilentlyContinue) -ne $null

if ($genNinja) {
    $cfg = 'Release'
    cmd /c "`"$vcvars`" && `"$cmake`" -S `"$root`" -B `"$root\build`" -G Ninja -DCMAKE_BUILD_TYPE=$cfg && `"$cmake`" --build `"$root\build`" --config $cfg"
} else {
    cmd /c "`"$vcvars`" && `"$cmake`" -S `"$root`" -B `"$root\build`" && `"$cmake`" --build `"$root\build`" --config Release"
}
if ($LASTEXITCODE -eq 0) {
    Write-Host ''
    Write-Host 'Build OK:'
    Get-ChildItem "$root\build" -Recurse -Filter SmartRAM.exe | ForEach-Object { $_.FullName }
}
