<#
  make_release.ps1 - stages bin\ into a versioned release package.

  Called by release.bat; normally you should run that instead. All the work
  lives here because cmd cannot compute SHA-256 hashes or read PE version
  resources without contortions, while PowerShell does both natively.

  What it produces, under <root>\release\:
      Zanphar-WinSuite-v<version>\           the package: exes, docs,
                                             MANIFEST.txt, SHA256SUMS.txt
      Zanphar-WinSuite-v<version>.zip        the package, zipped
      Zanphar-WinSuite-v<version>.zip.sha256 checksum of the zip itself

  -OutDir overrides where the package is written (default: <root>\release).

  Older Zanphar-WinSuite-v* packages are handled in two stages. Before anything
  is staged, each one is probed for open files: if any is in use the run stops
  there and writes nothing. Once the new package, archive and checksum are
  written, those older packages are removed, so release\ never holds more than
  the version just built. Nothing else is touched and the current version's
  files are never removed. If a lock appears between the probe and the
  removal, the run fails and names it: this version's package is complete, but
  release\ holds more than one version until the file is freed and the
  packaging step is run again.

  Exits 0 on success, 1 on any failure.
#>

[CmdletBinding()]
param(
    [string]$Root,
    [string]$Version,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$zip = $null

function Write-Utf8NoBom {
    param([string]$Path, [string[]]$Lines, [string]$NewLine = "`r`n")
    $text = ($Lines -join $NewLine) + $NewLine
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $text, $utf8)
}

function Get-Sha256 {
    param([string]$Path)
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Format-Size {
    param([long]$Bytes)
    if ($Bytes -ge 1MB) { '{0:N2} MiB' -f ($Bytes / 1MB) }
    elseif ($Bytes -ge 1KB) { '{0:N1} KiB' -f ($Bytes / 1KB) }
    else { "$Bytes bytes" }
}

function Get-OlderArtifacts {
    param([string]$Dir, [string[]]$Keep)
    # Superseded packages only: a folder or an archive named Zanphar-WinSuite-v*,
    # never this version's artefacts, never anything else in the folder.
    $found = @()
    if (-not (Test-Path -LiteralPath $Dir)) { return $found }
    foreach ($item in @(Get-ChildItem -LiteralPath $Dir -Force)) {
        if ($Keep -contains $item.Name) { continue }
        $olderPackage = $item.PSIsContainer -and $item.Name -like 'Zanphar-WinSuite-v*'
        $olderArchive = (-not $item.PSIsContainer) -and
                        $item.Name -match '^Zanphar-WinSuite-v.*\.zip(\.sha256)?$'
        if ($olderPackage -or $olderArchive) { $found += $item }
    }
    return $found
}

function Test-ArtifactInUse {
    param([System.IO.FileSystemInfo]$Item)
    # Removal needs every file in the item to be free. Opening each one read-only while
    # denying all sharing fails while any other handle exists (a zip open in Explorer,
    # a scanner reading it), and a read-only attribute is irrelevant because Remove-Item
    # clears it. Returns the name of the first busy file, or $null when all are free.
    $files = if ($Item.PSIsContainer) {
        @(Get-ChildItem -LiteralPath $Item.FullName -Recurse -Force -File)
    } else {
        @($Item)
    }
    foreach ($file in $files) {
        try {
            $probe = [System.IO.File]::Open(
                $file.FullName, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
            $probe.Dispose()
        }
        catch {
            return $file.Name
        }
    }
    return $null
}

try {
    if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
    $Root = (Resolve-Path -LiteralPath $Root).Path

    if (-not $Version) {
        $vf = Join-Path $Root 'VERSION'
        if (Test-Path -LiteralPath $vf) {
            $Version = (Get-Content -LiteralPath $vf | Where-Object { $_.Trim() } | Select-Object -First 1).Trim()
        }
    }
    if (-not $Version) { throw 'no version given and no VERSION file found' }
    if ($Version -notmatch '^[0-9][0-9A-Za-z.\-]*$') { throw "invalid version '$Version'" }

    $bin = Join-Path $Root 'bin'
    if (-not (Test-Path -LiteralPath $bin)) { throw "no bin\ folder - run build_all.bat first" }
    $exes = @(Get-ChildItem -LiteralPath $bin -Filter '*.exe' -File | Sort-Object Name)
    if ($exes.Count -eq 0) { throw 'bin\ has no .exe files - run build_all.bat first' }

    if (-not $OutDir) { $OutDir = Join-Path $Root 'release' }
    $packageName = "Zanphar-WinSuite-v$Version"
    $pkg = Join-Path $OutDir $packageName
    $zip = Join-Path $OutDir "$packageName.zip"
    $zipSum = "$zip.sha256"

    if (-not (Test-Path -LiteralPath $OutDir)) {
        New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    }

    $keep = @($packageName, "$packageName.zip", "$packageName.zip.sha256")

    # ---------------------------------------------------------- preflight locks
    # Deleting an older package needs every file in it to be free. Probing before
    # anything is written means a run that could not finish stops here, with the
    # release folder exactly as it was, instead of staging a new package and failing
    # afterwards. A lock that appears after this check is still caught by the prune
    # itself, which fails the run the same way.
    $blocked = New-Object System.Collections.Generic.List[string]
    foreach ($item in @(Get-OlderArtifacts -Dir $OutDir -Keep $keep)) {
        $busy = Test-ArtifactInUse -Item $item
        if ($busy) { $blocked.Add("$($item.Name) - $busy is in use") }
    }
    if ($blocked.Count -gt 0) {
        Write-Host ''
        Write-Host 'RELEASE ERROR: these older packages are in use and cannot be removed:' -ForegroundColor Red
        foreach ($line in $blocked) { Write-Host "  $line" -ForegroundColor Red }
        Write-Host 'Close whatever is using them (an archive open in Explorer, a running' -ForegroundColor Red
        Write-Host 'antivirus scan) and run release.bat again. Nothing was written.' -ForegroundColor Red
        exit 1
    }

    # Replace only this version's artefacts; other versions are left alone.
    foreach ($stale in @($pkg, $zip, $zipSum)) {
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
    }
    New-Item -ItemType Directory -Path $pkg -Force | Out-Null

    # ------------------------------------------------------------------ stage
    $exeNames = @()
    foreach ($e in $exes) {
        Copy-Item -LiteralPath $e.FullName -Destination (Join-Path $pkg $e.Name) -Force
        $exeNames += $e.Name
    }
    $docNames = @()
    foreach ($doc in @('README.md', 'CHANGES.md', 'LICENSE.md')) {
        $src = Join-Path $Root $doc
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $pkg $doc) -Force
            $docNames += $doc
        }
    }

    # ---------------------------------------------------------------- inspect
    $programs = foreach ($name in $exeNames) {
        $path = Join-Path $pkg $name
        $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        [pscustomobject]@{
            File        = $name
            Product     = if ($info.ProductName) { $info.ProductName } else { [System.IO.Path]::GetFileNameWithoutExtension($name) }
            Version     = if ($info.FileVersion) { $info.FileVersion } else { 'n/a' }
            ProductVer  = if ($info.ProductVersion) { $info.ProductVersion } else { 'n/a' }
            Company     = if ($info.CompanyName) { $info.CompanyName } else { 'n/a' }
            Description = if ($info.FileDescription) { $info.FileDescription } else { 'n/a' }
            Bytes       = (Get-Item -LiteralPath $path).Length
            Sha256      = Get-Sha256 -Path $path
        }
    }
    $programs = @($programs)
    $totalBytes = ($programs | Measure-Object -Property Bytes -Sum).Sum

    # --------------------------------------------------------------- manifest
    $stamp = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz')
    $m = New-Object System.Collections.Generic.List[string]
    $m.Add("Zanphar's WinSuite - release manifest")
    $m.Add('')
    $m.Add("suite version : $Version")
    $m.Add("package       : $packageName.zip")
    $m.Add("generated     : $stamp")
    $m.Add("programs      : $($programs.Count)")
    $m.Add("total size    : $(Format-Size $totalBytes) ($('{0:N0}' -f $totalBytes) bytes)")
    $m.Add('')
    $m.Add('Every program in this package was built by build_all.bat as a static,')
    $m.Add('self-contained x86-64 executable: the C++ runtime is linked in, so no')
    $m.Add('MinGW or Visual C++ redistributable is needed, and that build refuses to')
    $m.Add('produce an output that depends on one. Only Windows-shipped DLLs remain.')
    $m.Add('')
    $m.Add('-' * 78)
    $m.Add("PROGRAMS ($($programs.Count))")
    $m.Add('-' * 78)
    foreach ($p in $programs) {
        $m.Add('')
        $m.Add($p.Product)
        $m.Add("  file         : $($p.File)")
        $m.Add("  version      : $($p.Version) (product $($p.ProductVer))")
        $m.Add("  size         : $('{0:N0}' -f $p.Bytes) bytes ($(Format-Size $p.Bytes))")
        $m.Add("  description  : $($p.Description)")
        $m.Add("  publisher    : $($p.Company)")
        $m.Add("  sha256       : $($p.Sha256)")
    }
    $m.Add('')
    $m.Add('-' * 78)
    $m.Add("DOCUMENTS ($($docNames.Count))")
    $m.Add('-' * 78)
    foreach ($d in $docNames) { $m.Add("  $d") }
    $m.Add('')
    $m.Add('-' * 78)
    $m.Add('VERIFY')
    $m.Add('-' * 78)
    $m.Add('  sha256sum -c SHA256SUMS.txt            (Git Bash / MSYS2 / Linux)')
    $m.Add('  certutil -hashfile <file> SHA256       (cmd.exe, one file at a time)')
    $m.Add('  Get-FileHash -Algorithm SHA256 .\<file> (PowerShell)')
    $m.Add('')
    $m.Add('  The archive itself: sha256sum -c <package>.zip.sha256 (in release\)')
    Write-Utf8NoBom -Path (Join-Path $pkg 'MANIFEST.txt') -Lines $m.ToArray()

    # --------------------------------------------------------------- checksums
    $sumLines = New-Object System.Collections.Generic.List[string]
    $files = @(Get-ChildItem -LiteralPath $pkg -File | Sort-Object Name)
    foreach ($f in $files) {
        # two spaces = text mode in coreutils' sha256sum -c
        $sumLines.Add("$(Get-Sha256 -Path $f.FullName)  $($f.Name)")
    }
    # Checksum files are LF-only: coreutils' sha256sum -c treats a trailing CR
    # as part of the file name and fails to match every entry.
    Write-Utf8NoBom -Path (Join-Path $pkg 'SHA256SUMS.txt') -Lines $sumLines.ToArray() -NewLine "`n"

    # -------------------------------------------------------------------- zip
    # Entries are written by hand so that each name uses a forward slash, as
    # the ZIP spec requires. Both Compress-Archive and ZipFile's directory
    # helper on Windows PowerShell 5.1 (.NET Framework) store 'package\file',
    # which makes macOS/Linux extractors create one file literally named
    # 'package\file.exe' instead of a folder.
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipBytes = [System.IO.File]::Open($zip, [System.IO.FileMode]::Create)
    try {
        $archive = New-Object System.IO.Compression.ZipArchive(
            $zipBytes, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($f in @(Get-ChildItem -LiteralPath $pkg -File | Sort-Object Name)) {
                $entryName = "$packageName/$($f.Name)"
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $archive, $f.FullName, $entryName,
                    [System.IO.Compression.CompressionLevel]::Optimal)
            }
        }
        finally { $archive.Dispose() }
    }
    finally { $zipBytes.Dispose() }
    if (-not (Test-Path -LiteralPath $zip)) { throw 'the archive was not produced' }
    $zipHash = Get-Sha256 -Path $zip
    Write-Utf8NoBom -Path $zipSum -Lines @("$zipHash *$packageName.zip") -NewLine "`n"

    # ------------------------------------------------------------ prune older
    # Only once this version's package, archive and checksum are safely on disk
    # are the superseded ones removed - a failed run leaves the previous release
    # exactly where it was. The match is deliberately narrow: folders and
    # archives named Zanphar-WinSuite-v*, never anything else in release\ and
    # never this version's own three artefacts. A file that cannot be removed
    # (open in Explorer, for instance) is reported but does not fail the build,
    # because the package just written is complete and valid.
    $pruned = New-Object System.Collections.Generic.List[string]
    $stuck = New-Object System.Collections.Generic.List[string]
    foreach ($item in @(Get-OlderArtifacts -Dir $OutDir -Keep $keep)) {
        try {
            Remove-Item -LiteralPath $item.FullName -Recurse -Force
            $pruned.Add($item.Name)
        }
        catch {
            $stuck.Add("$($item.Name) ($($_.Exception.Message))")
        }
    }

    # An older package that survives leaves release\ with two versions, which is the
    # state this step exists to prevent, so the run fails instead of reporting success
    # over a stale package. This version's package is left where it is: it is complete
    # and verified, so once the old file is freed a rerun only has to prune.
    if ($stuck.Count -gt 0) {
        Write-Host ''
        Write-Host 'RELEASE ERROR: these older packages could not be removed:' -ForegroundColor Red
        foreach ($name in $stuck) { Write-Host "  $name" -ForegroundColor Red }
        Write-Host 'Close whatever is using them (an archive open in Explorer, a running' -ForegroundColor Red
        Write-Host 'antivirus scan), then run release.bat again: the new package is complete,' -ForegroundColor Red
        Write-Host 'but release\ still holds more than one version.' -ForegroundColor Red
        exit 1
    }

    # ----------------------------------------------------------------- report
    $wName = [Math]::Max(20, (($programs | ForEach-Object { $_.Product.Length } | Measure-Object -Maximum).Maximum))
    $head = '  ' + 'program'.PadRight($wName) + '  ' + 'version'.PadRight(10) + '  ' + 'size'.PadLeft(10) + '  ' + 'sha256'
    Write-Host ''
    Write-Host $head
    Write-Host ('  ' + ('-' * $wName) + '  ' + ('-' * 10) + '  ' + ('-' * 10) + '  ' + ('-' * 16))
    foreach ($p in $programs) {
        $line = '  ' + $p.Product.PadRight($wName) + '  ' + $p.Version.PadRight(10) + '  ' +
                (Format-Size $p.Bytes).PadLeft(10) + '  ' + $p.Sha256.Substring(0, 16)
        Write-Host $line
    }
    Write-Host ''
    Write-Host "  package : $pkg"
    Write-Host "  archive : $zip"
    Write-Host "  sha256  : $zipHash"
    Write-Host "  checksum: $zipSum"
    if ($pruned.Count -gt 0) {
        Write-Host "  pruned  : $($pruned.Count) older item(s) from $OutDir"
        foreach ($name in $pruned) { Write-Host "            $name" }
    }
    exit 0
}
catch {
    # Do not leave a half-written archive behind: a zero-byte or truncated zip
    # looks like a real release to the next person who opens release\.
    if ($zip -and (Test-Path -LiteralPath $zip)) {
        try { Remove-Item -LiteralPath $zip -Force } catch { }
    }
    Write-Host ''
    Write-Host "RELEASE ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
