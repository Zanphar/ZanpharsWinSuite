# tools/package.ps1 — build SmartRAM and package the exe + README into a zip.
param(
    [ValidateSet('MinGW', 'MSVC')]
    [string]$Toolchain = 'MinGW'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'

if ($Toolchain -eq 'MinGW') {
    & (Join-Path $PSScriptRoot 'build-mingw.ps1')
    $exe = Join-Path $root 'build-mingw\SmartRAM.exe'
} else {
    & (Join-Path $PSScriptRoot 'build.ps1')
    $exe = Join-Path $root 'build\SmartRAM.exe'
}
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (-not (Test-Path $exe)) { throw "Built executable not found: $exe" }

# Version comes from the Windows resource so the archive name tracks the app.
$version = '1.0.0.0'
$rcPath = Join-Path $root 'res\app.rc'
$m = [regex]::Match((Get-Content $rcPath -Raw),
                    'FILEVERSION\s+([0-9]+(?:\s*,\s*[0-9]+){3})')
if ($m.Success) {
    $version = ($m.Groups[1].Value -replace '\s', '') -replace ',', '.'
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
$zip = Join-Path $dist "SmartRAM-$version-win64.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("SmartRAM-package-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $stage | Out-Null

try {
    Copy-Item $exe (Join-Path $stage 'SmartRAM.exe')
    Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'README.md')
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal -Force
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $names = $archive.Entries | ForEach-Object { $_.FullName }
} finally {
    $archive.Dispose()
}

$hash = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
$checksum = Join-Path $dist ([System.IO.Path]::GetFileName($zip) + '.sha256')
# LF (not CRLF) so sha256sum and other Unix-style tools read the file directly.
[System.IO.File]::WriteAllText($checksum,
    "$hash  $([System.IO.Path]::GetFileName($zip))`n",
    [System.Text.Encoding]::ASCII)

Write-Host ''
Write-Host 'Package OK:'
Write-Host "  $zip"
Write-Host "  $checksum"
Write-Host ("  contains: " + ($names -join ', '))
