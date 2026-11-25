<#
.SYNOPSIS
    Compares every Home Policy catalog row with the ADMX files Windows ships.

.DESCRIPTION
    Reads HomePolicy\src\catalog.cpp and, for each policy value it declares, finds
    the policy definition in the ADMX files under C:\Windows\PolicyDefinitions and
    compares four things:

      key      the registry path the ADMX declares (without the hive)
      value    the value name the ADMX declares
      scope    the policy's class (Machine -> HKEY_LOCAL_MACHINE, User -> HKCU)
      data     the enabled/disabled values, or the allowed range/items

    The catalog is effect-oriented: a row titled "Turn off X" writes the policy's
    *disabled* data as its own "on". So a pair that matches in the other order is
    reported as ok (inverted) rather than as drift - what would be drift is a
    number the ADMX never mentions.

    Read-only: nothing is written to the registry and no network is used.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File HomePolicy\tools\check-admx.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File HomePolicy\tools\check-admx.ps1 -AdmxPath C:\Windows\PolicyDefinitions -Verbose
#>
param(
    [string]$AdmxPath = 'C:\Windows\PolicyDefinitions',
    [string]$CatalogPath,
    # Extra folders to read ADMX from - Edge's policies live in MSEdge.admx,
    # which Windows does not ship.
    [string[]]$ExtraAdmxPath = @()
)

# Values that are known not to be administrator-template settings. Each entry
# says why, so a reader can judge the exception instead of trusting it.
$NoAdmxReason = @{
    'Enabled'                   = 'documented registry setting (Windows Script Host), not an administrator template'
    'HideFirstRunExperience'    = 'Edge policy - MSEdge.admx, not shipped with Windows'
    'HubsSidebarEnabled'        = 'Edge policy - MSEdge.admx, not shipped with Windows'
    'EdgeShoppingAssistantEnabled' = 'Edge policy - MSEdge.admx, not shipped with Windows'
    'ShowRecommendationsEnabled'   = 'Edge policy - MSEdge.admx, not shipped with Windows'
}

$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (-not $CatalogPath) { $CatalogPath = Join-Path $root 'HomePolicy\src\catalog.cpp' }

if (-not (Test-Path $AdmxPath)) {
    Write-Output "No ADMX directory at $AdmxPath - nothing to compare against."
    exit 2
}
if (-not (Test-Path $CatalogPath)) {
    Write-Output "No catalog at $CatalogPath."
    exit 2
}

# --------------------------------------------------------------- the catalog
# The table is a plain initializer list, so it is read as text: every entry starts
# at four spaces with "{<category>, L"" and runs to the matching brace.
function Read-Catalog([string]$path) {
    $text = Get-Content $path -Raw
    $start = $text.IndexOf('const std::vector<Policy> kPolicies = {')
    if ($start -lt 0) { throw 'kPolicies initializer not found in the catalog' }
    $body = $text.Substring($start)
    $end = $body.IndexOf("`n};")
    if ($end -gt 0) { $body = $body.Substring(0, $end) }

    $rows = @()
    $script:CatalogRows = 0
    $chunks = [regex]::Split($body, "(?m)^    \{(?=\d+, L"")")
    foreach ($chunk in $chunks) {
        if ($chunk -notmatch '^\d+, L"') { continue }
        ++$script:CatalogRows
        $title = [regex]::Match($chunk, '^\d+, L"((?:[^"\\]|\\.)*)"').Groups[1].Value
        $scope = [regex]::Match($chunk, 'HKEY_LOCAL_MACHINE|HKEY_CURRENT_USER')
        $afterScope = $chunk.Substring($scope.Index + $scope.Length)
        $key = [regex]::Match($afterScope, 'L"((?:[^"\\]|\\.)*)"').Groups[1].Value

        $values = @()
        foreach ($m in [regex]::Matches($chunk, '\{L"([^"]+)",\s*(\d+),\s*(\d+),\s*(true|false)\}')) {
            $values += [pscustomobject]@{
                Value  = $m.Groups[1].Value
                On     = $m.Groups[2].Value
                Off    = $m.Groups[3].Value
                HasOff = ($m.Groups[4].Value -eq 'true')
            }
        }
        if (-not $values.Count) { continue }

        # The trailing "L"effect", L"caveat"|nullptr" pair, if the row has one.
        $caveat = ''
        $tail = [regex]::Match($chunk, ',\s*L"((?:[^"\\]|\\.)*)"\s*,\s*(?:(L)"((?:[^"\\]|\\.)*)")?')
        if ($tail.Success -and $tail.Groups[3].Success) { $caveat = $tail.Groups[3].Value }

        foreach ($value in $values) {
            $rows += [pscustomobject]@{
                Title  = $title
                Scope  = $scope.Value
                Key    = $key
                Value  = $value.Value
                On     = $value.On
                Off    = $value.Off
                HasOff = $value.HasOff
                Caveat = $caveat
            }
        }
    }
    return $rows
}

# ------------------------------------------------------------------ the ADMX
function Normalize-Key([string]$key) {
    if (-not $key) { return '' }
    $k = $key.Replace('/', '\').Trim()
    while ($k.Contains('\\')) { $k = $k.Replace('\\', '\') }
    return $k.TrimEnd('\').ToLowerInvariant()
}

function Read-Admx([string]$directory) {
    $index = @{}
    $files = Get-ChildItem -Path $directory -Filter '*.admx' -Recurse -ErrorAction SilentlyContinue
    foreach ($file in $files) {
        $xml = New-Object System.Xml.XmlDocument
        try { $xml.Load($file.FullName) } catch { continue }
        foreach ($policy in $xml.SelectNodes("//*[local-name()='policy']")) {
            $class = $policy.GetAttribute('class')
            $key = $policy.GetAttribute('key')

            $valueName = $policy.GetAttribute('valueName')
            if ($valueName) {
                $on = '1'
                $off = '0'
                $enabled = $policy.SelectSingleNode("*[local-name()='enabledValue']/*")
                $disabled = $policy.SelectSingleNode("*[local-name()='disabledValue']/*")
                if ($enabled) { $on = $enabled.GetAttribute('value') }
                if ($disabled) { $off = $disabled.GetAttribute('value') }
                Add-Entry $index $valueName $key $class 'policy' $on $off $null $null @() $file.Name
            }

            foreach ($element in $policy.SelectNodes("*[local-name()='elements']/*")) {
                $name = $element.GetAttribute('valueName')
                if (-not $name) { continue }
                $elementKey = $element.GetAttribute('key')
                if (-not $elementKey) { $elementKey = $key }
                $kind = $element.LocalName
                $on = $null; $off = $null; $min = $null; $max = $null; $items = @()
                switch ($kind) {
                    'boolean' {
                        $trueNode = $element.SelectSingleNode("*[local-name()='trueValue']")
                        $falseNode = $element.SelectSingleNode("*[local-name()='falseValue']")
                        $on = if ($trueNode) { $trueNode.GetAttribute('value') } else { '1' }
                        $off = if ($falseNode) { $falseNode.GetAttribute('value') } else { '0' }
                    }
                    'decimal' {
                        $min = $element.GetAttribute('minValue')
                        $max = $element.GetAttribute('maxValue')
                    }
                    'enum' {
                        # <item><value><decimal value="2"/></value></item>
                        foreach ($item in $element.SelectNodes("*[local-name()='item']")) {
                            $valueNode = $item.SelectSingleNode("*[local-name()='value']/*")
                            if ($valueNode) { $items += $valueNode.GetAttribute('value') }
                        }
                    }
                }
                Add-Entry $index $name $elementKey $class $kind $on $off $min $max $items $file.Name
            }
        }
    }
    return [pscustomobject]@{ Index = $index; Files = $files.Count }
}

function Add-Entry($index, $value, $key, $class, $kind, $on, $off, $min, $max, $items, $file) {
    $name = $value.ToLowerInvariant()
    if (-not $index.ContainsKey($name)) { $index[$name] = @() }
    $index[$name] += [pscustomobject]@{
        Key = $key; NormKey = (Normalize-Key $key); Class = $class; Kind = $kind
        On = $on; Off = $off; Min = $min; Max = $max; Items = $items; File = $file
    }
}

function Scope-Of([string]$class) {
    switch ($class) {
        'Machine' { return 'HKEY_LOCAL_MACHINE' }
        'User'    { return 'HKEY_CURRENT_USER' }
        default   { return 'both' }
    }
}

# ---------------------------------------------------------------- comparison
function Compare-Row($row, $candidates) {
    $wanted = @($candidates | Where-Object {
        $_.NormKey -eq (Normalize-Key $row.Key) -and
        ((Scope-Of $_.Class) -eq $row.Scope -or (Scope-Of $_.Class) -eq 'both')
    })
    if (-not $wanted.Count) {
        $otherScope = @($candidates | Where-Object { $_.NormKey -eq (Normalize-Key $row.Key) })
        if ($otherScope.Count) {
            return @{ Status = 'SCOPE-DRIFT'
                      Detail = ("ADMX declares this value for {0}, the catalog writes {1}" -f
                                (($otherScope | ForEach-Object { Scope-Of $_.Class }) -join '/'), $row.Scope) }
        }
        return @{ Status = 'KEY-DRIFT'
                  Detail = ("catalog: {0}\ADMX: {1}" -f $row.Key,
                            (($candidates | ForEach-Object { $_.Key }) -join ' ; ')) }
    }

    $entry = $wanted[0]
    switch ($entry.Kind) {
        'policy' {
            $pair = @($entry.On, $entry.Off)
            if ($row.HasOff) {
                # -join binds tighter than -eq, so both sides are joined first and
                # only then compared; the one-liner version this replaced never
                # compared anything at all and passed every row.
                $ours = (@($row.On, $row.Off) | Sort-Object) -join ','
                $theirs = (@($pair) | Sort-Object) -join ','
                if ($ours -eq $theirs) {
                    $shape = if ($row.On -eq $entry.On) { 'data {0}/{1}' -f $entry.On, $entry.Off }
                             else { 'data {0}/{1}, inverted' -f $entry.On, $entry.Off }
                    return @{ Status = 'ok'; Detail = $shape }
                }
                return @{ Status = 'DATA-DRIFT'; Detail = ("catalog {0}/{1}, ADMX {2}/{3}" -f $row.On, $row.Off, $entry.On, $entry.Off) }
            }
            if ($row.On -eq $entry.On -or $row.On -eq $entry.Off) {
                return @{ Status = 'ok'; Detail = ("data {0}/{1}, deletes on disable" -f $entry.On, $entry.Off) }
            }
            return @{ Status = 'DATA-DRIFT'; Detail = ("catalog {0}, ADMX {1}/{2}" -f $row.On, $entry.On, $entry.Off) }
        }
        'boolean' {
            if ($row.On -eq $entry.On -or $row.On -eq $entry.Off) {
                return @{ Status = 'ok'; Detail = ("boolean {0}/{1}" -f $entry.On, $entry.Off) }
            }
            return @{ Status = 'DATA-DRIFT'; Detail = ("catalog {0}, ADMX boolean {1}/{2}" -f $row.On, $entry.On, $entry.Off) }
        }
        'enum' {
            if ($entry.Items -contains $row.On -and (-not $row.HasOff -or $entry.Items -contains $row.Off)) {
                return @{ Status = 'ok'; Detail = ("enum values {0}" -f ($entry.Items -join '/')) }
            }
            return @{ Status = 'DATA-DRIFT'; Detail = ("catalog {0}/{1}, ADMX items {2}" -f $row.On, $row.Off, ($entry.Items -join '/')) }
        }
        'decimal' {
            $ok = $true
            if ($entry.Min -and [int]$row.On -lt [int]$entry.Min) { $ok = $false }
            if ($entry.Max -and [int]$row.On -gt [int]$entry.Max) { $ok = $false }
            if ($ok) {
                $range = if ($entry.Min -or $entry.Max) { "range {0}-{1}" -f $entry.Min, $entry.Max } else { 'no range declared' }
                return @{ Status = 'ok'; Detail = $range }
            }
            return @{ Status = 'DATA-DRIFT'; Detail = ("catalog {0}, ADMX range {1}-{2}" -f $row.On, $entry.Min, $entry.Max) }
        }
        default {
            return @{ Status = 'ok'; Detail = ("{0} element, data not compared" -f $entry.Kind) }
        }
    }
}

# --------------------------------------------------------------------- report
$catalog = @(Read-Catalog $CatalogPath)
$admx = Read-Admx $AdmxPath
$extraFiles = 0
foreach ($extra in $ExtraAdmxPath) {
    if (-not (Test-Path $extra)) { continue }
    $more = Read-Admx $extra
    foreach ($name in $more.Index.Keys) {
        if (-not $admx.Index.ContainsKey($name)) { $admx.Index[$name] = @() }
        $admx.Index[$name] += $more.Index[$name]
    }
    $extraFiles += $more.Files
}

Write-Output ''
Write-Output 'Home Policy - ADMX drift report'
Write-Output ("  catalog : {0} ({1} rows, {2} values)" -f $CatalogPath, $script:CatalogRows, $catalog.Count)
Write-Output ("  admx    : {0} ({1} files{2})" -f $AdmxPath, $admx.Files,
              $(if ($extraFiles) { ", plus $extraFiles extra" } else { '' }))
Write-Output ''

$counts = @{}
$lines = @()
foreach ($row in $catalog) {
    $candidates = $admx.Index[$row.Value.ToLowerInvariant()]
    $matches = @($candidates | Where-Object { $_.NormKey -eq (Normalize-Key $row.Key) })
    if ($NoAdmxReason.ContainsKey($row.Value) -and -not $matches.Count) {
        $result = @{ Status = 'no-admx (known)'; Detail = $NoAdmxReason[$row.Value] }
    } elseif (-not $candidates) {
        $result = @{ Status = 'NO-ADMX'; Detail = 'no policy definition declares this value name' }
    } else {
        $result = Compare-Row $row $candidates
    }

    $counts[$result.Status] = 1 + [int]$counts[$result.Status]
    $detail = $result.Detail
    if ($result.Status -ne 'ok') { $detail = "'{0}' - {1}" -f $row.Title, $detail }
    $lines += [pscustomobject]@{
        Status = $result.Status
        Value  = $row.Value
        Row    = $row.Title
        Detail = $detail
    }
}

foreach ($line in ($lines | Sort-Object Status, Value)) {
    Write-Output ("  {0,-18} {1,-42} {2}" -f $line.Status, $line.Value, $line.Detail)
}

$drift = @('KEY-DRIFT', 'SCOPE-DRIFT', 'DATA-DRIFT', 'NO-ADMX') |
    Where-Object { $counts.ContainsKey($_) } |
    ForEach-Object { '{0}: {1}' -f $_, $counts[$_] }
$ok = [int]$counts['ok'] + [int]$counts['no-admx (known)']

Write-Output ''
if ($drift) {
    Write-Output ("DRIFT - {0} value(s) agree, {1}" -f $ok, ($drift -join ', '))
    exit 1
}
Write-Output ("CLEAN - all {0} value(s) agree with the ADMX files" -f $ok)
exit 0
