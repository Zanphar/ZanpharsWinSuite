<#
  verify_binaries.ps1 - proves that the executables in bin\ depend only on
  DLLs Windows itself ships.

  This is the CI half of the promise build_all.bat makes. build_all.bat
  already fails a build whose output imports libgcc, libstdc++ or
  libwinpthread; this script checks the finished folder independently, with
  an allowlist of operating-system DLLs, so a stale or hand-copied binary in
  bin\ cannot slip through unnoticed.

  Usage:
      tools\verify_binaries.ps1
      tools\verify_binaries.ps1 -BinDir bin -Objdump C:\mingw64\bin\objdump.exe
      tools\verify_binaries.ps1 -Expected BrowserCleaner.exe,DNSProtect.exe

  Exit code 0 when every executable is self-contained, 1 otherwise.
#>

[CmdletBinding()]
param(
    [string]$BinDir,
    [string]$Objdump,
    [string[]]$Expected = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# DLLs that ship with Windows. Everything else - libgcc_s_seh-1.dll,
# libstdc++-6.dll, libwinpthread-1.dll, a third-party runtime - means the
# executable is not self-contained.
$OsDlls = @(
    'advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'comdlg32.dll', 'crypt32.dll',
    'd2d1.dll', 'd3d11.dll', 'dbghelp.dll', 'dnsapi.dll', 'dwmapi.dll',
    'dwrite.dll', 'dxgi.dll', 'gdi32.dll', 'gdiplus.dll', 'imm32.dll',
    'iphlpapi.dll', 'kernel32.dll', 'msimg32.dll', 'msvcrt.dll', 'ntdll.dll',
    'ole32.dll', 'oleacc.dll', 'oleaut32.dll', 'powrprof.dll', 'psapi.dll',
    'rpcrt4.dll', 'secur32.dll', 'setupapi.dll', 'shcore.dll', 'shell32.dll',
    'shlwapi.dll', 'ucrtbase.dll', 'user32.dll', 'userenv.dll', 'uxtheme.dll',
    'version.dll', 'wer.dll', 'winhttp.dll', 'wininet.dll', 'winmm.dll',
    'wintrust.dll', 'ws2_32.dll', 'wtsapi32.dll'
)
# API sets are virtual DLLs resolved by the loader from a contract name.
$OsDllPattern = '^api-ms-win-[a-z0-9\-]+\.dll$'

function Resolve-Objdump {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "objdump not found at '$Explicit'" }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }
    if ($env:MINGW_BIN) {
        $candidate = Join-Path $env:MINGW_BIN 'objdump.exe'
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    $onPath = Get-Command 'objdump.exe' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    foreach ($dir in @('C:\msys64\mingw64\bin', 'C:\mingw64\bin', "$env:ProgramFiles\mingw64\bin")) {
        $candidate = Join-Path $dir 'objdump.exe'
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw 'objdump.exe not found. Set MINGW_BIN, or pass -Objdump <path>.'
}

function Get-Imports {
    param([string]$ObjdumpPath, [string]$ExePath)

    $output = & $ObjdumpPath -p $ExePath 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "objdump could not read '$ExePath': $($output | Select-Object -First 1)"
    }
    $names = foreach ($line in $output) {
        if ("$line" -match '^\s*DLL Name:\s*(\S+)\s*$') { $Matches[1] }
    }
    return @($names)
}

function Test-OsDll {
    param([string]$Name)
    $lower = $Name.ToLowerInvariant()
    if ($OsDlls -contains $lower) { return $true }
    return [bool]($lower -match $OsDllPattern)
}

try {
    if (-not $BinDir) { $BinDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin' }
    if (-not (Test-Path -LiteralPath $BinDir)) { throw "no '$BinDir' folder - build the suite first" }
    $BinDir = (Resolve-Path -LiteralPath $BinDir).Path
    $objdump = Resolve-Objdump -Explicit $Objdump

    $exes = @(Get-ChildItem -LiteralPath $BinDir -Filter '*.exe' -File | Sort-Object Name)
    if ($exes.Count -eq 0) { throw "no .exe files in '$BinDir' - build the suite first" }

    Write-Host "Verifying $($exes.Count) executable(s) in $BinDir"
    Write-Host "  objdump: $objdump"
    Write-Host ''

    $problems = 0
    $rows = foreach ($exe in $exes) {
        $imports = Get-Imports -ObjdumpPath $objdump -ExePath $exe.FullName
        $foreign = @($imports | Where-Object { -not (Test-OsDll -Name $_) })
        $kb = [math]::Round($exe.Length / 1KB)

        if ($foreign.Count -gt 0) {
            $problems++
            Write-Host ('  [FAIL] {0}  {1} KB  - not self-contained' -f $exe.Name, $kb)
            foreach ($dll in ($foreign | Sort-Object)) {
                Write-Host "           needs $dll"
            }
            if ($foreign -match '^(libgcc|libstd|libruntime|libwinpthread)') {
                Write-Host '           -> rebuild with -static (build_all.bat does this)'
            }
        } else {
            Write-Host ('  [ OK ] {0}  {1} KB  - {2} imports, OS DLLs only' -f $exe.Name, $kb, $imports.Count)
        }
        [pscustomobject]@{ File = $exe.Name; Imports = $imports.Count; Foreign = $foreign.Count }
    }

    # -Expected arrives as one comma-separated string when the script is run
    # with -File, which is how CI runs it; split it here.
    foreach ($group in $Expected) {
        foreach ($wanted in ($group -split ',')) {
            $name = $wanted.Trim()
            if (-not $name) { continue }
            if (-not (Test-Path -LiteralPath (Join-Path $BinDir $name))) {
                Write-Host "  [FAIL] expected output missing: $name"
                $problems++
            }
        }
    }

    Write-Host ''
    if ($problems -gt 0) {
        Write-Host "FAILED: $problems problem(s) - see the [FAIL] lines above."
        exit 1
    }
    $totalImports = ($rows | Measure-Object -Property Imports -Sum).Sum
    Write-Host "SELF-CONTAINED: $($exes.Count)/$($exes.Count) executable(s), $totalImports DLLs, all provided by Windows."
    exit 0
}
catch {
    Write-Host ''
    Write-Host "VERIFY ERROR: $($_.Exception.Message)"
    exit 1
}
