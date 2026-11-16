# tools/settings_test.ps1 — exercise toggles + slider click + single-instance surfacing.
Add-Type -AssemblyName System.Drawing
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class Win3 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int max);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $sig

function Find-Hwnd([string]$cls) {
    $found = [IntPtr]::Zero
    $cb = {
        param($h, $lp)
        $cn = New-Object System.Text.StringBuilder 256
        [Win3]::GetClassName($h, $cn, 256) | Out-Null
        if ([string]$cn -eq $script:wantClass) { $script:found = $h; return $false }
        return $true
    }
    $script:wantClass = $cls
    [Win3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

$widget = Find-Hwnd 'SmartRAM_Widget_v1'
$settings = Find-Hwnd 'SmartRAM_Settings_v1'
if ($settings -eq [IntPtr]::Zero) { Write-Host 'settings NOT open'; exit 1 }
$rw = New-Object Win3+RECT
$rs = New-Object Win3+RECT
[Win3]::GetWindowRect($widget, [ref]$rw) | Out-Null
[Win3]::GetWindowRect($settings, [ref]$rs) | Out-Null
$ox = $rs.L; $oy = $rs.T
Write-Host ("settings at " + $rs.L + "," + $rs.T)

function Click-Design([float]$dx, [float]$dy) {
    [Win3]::SetCursorPos($ox + 30 + $dx, $oy + 30 + $dy) | Out-Null
    Start-Sleep -Milliseconds 120
    [Win3]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [Win3]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}

# 1. toggle Smart Clean OFF (toggle centre design x=47, y=52+23=75)
Click-Design 47 75
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_toggle_off.png" | Out-Null

# 2. toggle back ON
Click-Design 47 75

# 3. click low on the slider track (design x=120,y=108) -> threshold ~59%
Click-Design 120 108
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_slider_lo.png" | Out-Null

# 4. restore threshold to 85% (click right end: x=240)
Click-Design 240 108

# 5. single instance: launch second exe; card should replay its intro pop
Start-Process "$PSScriptRoot\..\build\SmartRAM.exe"
Start-Sleep -Milliseconds 900
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_single.png" | Out-Null
Write-Host 'settings/single-instance test shots saved'
