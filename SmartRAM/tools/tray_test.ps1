# tools/tray_test.ps1 — close-to-tray flow: click X, verify hidden, relaunch exe shows card.
Add-Type -AssemblyName System.Drawing
$sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win4 {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $sig

function Find-Hwnd([string]$cls) {
    $script:out = [IntPtr]::Zero
    $cb = {
        param($h, $lp)
        $cn = New-Object System.Text.StringBuilder 256
        [Win4]::GetClassName($h, $cn, 256) | Out-Null
        if ([string]$cn -eq $script:wantClass) { $script:out = $h; return $false }
        return $true
    }
    $script:wantClass = $cls
    [Win4]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:out
}

$h = Find-Hwnd 'SmartRAM_Widget_v1'
if ($h -eq [IntPtr]::Zero) { Write-Host 'widget NOT found'; exit 1 }
$r = New-Object Win4+RECT
[Win4]::GetWindowRect($h, [ref]$r) | Out-Null

# click the widget's X (design centre 281, 25 + 30 padding)
[Win4]::SetCursorPos($r.L + 30 + 281, $r.T + 30 + 25) | Out-Null
Start-Sleep -Milliseconds 150
[Win4]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 60
[Win4]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 900

$h2 = Find-Hwnd 'SmartRAM_Widget_v1'
$vis = if ($h2 -ne [IntPtr]::Zero) { [Win4]::IsWindowVisible($h2) } else { $false }
Write-Host ("after X click: widget hwnd exists=" + ($h2 -ne [IntPtr]::Zero) + " visible=" + $vis)
if ($vis) { Write-Host 'FAIL: still visible'; exit 1 }

# relaunch the exe (single-instance path posts WM_APP+4 -> resurfaces the card)
Start-Process "$PSScriptRoot\..\build\SmartRAM.exe"
Start-Sleep -Milliseconds 1200
$h3 = Find-Hwnd 'SmartRAM_Widget_v1'
$vis3 = if ($h3 -ne [IntPtr]::Zero) { [Win4]::IsWindowVisible($h3) } else { $false }
Write-Host ("after relaunch: visible=" + $vis3)
$procCount = @(Get-Process SmartRAM -ErrorAction SilentlyContinue).Count
Write-Host ("processes: " + $procCount)
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_final.png" | Out-Null
