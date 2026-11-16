# tools/build-mingw.ps1 — build SmartRAM with MinGW-w64 (no Visual Studio needed).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-Tool([string[]]$names) {
    foreach ($name in $names) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    return $null
}

$gxx     = Find-Tool @('x86_64-w64-mingw32-g++', 'g++')
$windres = Find-Tool @('x86_64-w64-mingw32-windres', 'windres')
$cmake   = Find-Tool @('cmake')
$ninja   = Find-Tool @('ninja')

if (-not $gxx)     { throw 'MinGW-w64 g++ not found on PATH.' }
if (-not $windres) { throw 'MinGW-w64 windres not found on PATH.' }
if (-not $cmake)   { throw 'CMake not found on PATH.' }
if (-not $ninja)   { throw 'Ninja not found on PATH.' }

$build = Join-Path $root 'build-mingw'
$cmakeArgs = @(
    '-S', $root,
    '-B', $build,
    '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_CXX_COMPILER=$gxx",
    "-DCMAKE_RC_COMPILER=$windres"
)

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

& $cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }

Write-Host ''
Write-Host 'Build OK:'
Get-ChildItem $build -Recurse -Filter SmartRAM.exe |
    ForEach-Object { $_.FullName }
