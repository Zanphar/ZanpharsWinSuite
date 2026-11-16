# tools/auto_test.ps1 — scripted interaction test for the widget.
Add-Type -AssemblyName System.Drawing
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $sig

function Click-At([int]$x, [int]$y) {
    [Win]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 120
    [Win]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)   # down
    Start-Sleep -Milliseconds 60
    [Win]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)   # up
}

$p = Get-Process SmartRAM -ErrorAction SilentlyContinue
$h = [IntPtr]::Zero
if ($p -and $p.MainWindowHandle -ne 0) { $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { $h = [Win]::FindWindowW('SmartRAM_Widget_v1', $null) }
if ($h -eq [IntPtr]::Zero) { Write-Host 'widget window NOT found'; exit 1 }
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Host ("widget at " + $r.L + "," + $r.T + " size " + ($r.R-$r.L) + "x" + ($r.B-$r.T))
$ox = $r.L; $oy = $r.T

# hover over card (no zone yet) — then screenshot chrome fade
[Win]::SetCursorPos(($ox + 158), ($oy + 100)) | Out-Null
Start-Sleep -Milliseconds 600
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_hover.png" | Out-Null

# hover gear (design 247,25 => + padding 30)
[Win]::SetCursorPos(($ox + 30 + 247), ($oy + 30 + 25)) | Out-Null
Start-Sleep -Milliseconds 500
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_chrome.png" | Out-Null

# click gear (design 247, 25)
Click-At ($ox + 30 + 247) ($oy + 30 + 25)
Start-Sleep -Milliseconds 800
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_settings.png" | Out-Null

# settings window position?
$h2 = [Win]::FindWindowW('SmartRAM_Settings_v1', $null)
if ($h2 -eq [IntPtr]::Zero) {
    $p2 = Get-Process SmartRAM -ErrorAction SilentlyContinue
    # enumerate top windows of the process instead
    $h2 = [Win]::FindWindowW('SmartRAM_Settings_v1', $null)
}
if ($h2 -ne [IntPtr]::Zero) {
    $r2 = New-Object Win+RECT
    [Win]::GetWindowRect($h2, [ref]$r2) | Out-Null
    Write-Host ("settings at " + $r2.L + "," + $r2.T + " " + ($r2.R-$r2.L) + "x" + ($r2.B-$r2.T))
} else { Write-Host "settings window NOT open" }
