# tools/clean_test.ps1 — click CLEAN NOW and capture the spinner, then the card afterwards.
Add-Type -AssemblyName System.Drawing
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class Win2 {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
Add-Type -TypeDefinition $sig

$h = [Win2]::FindWindowW('SmartRAM_Widget_v1', $null)
if ($h -eq [IntPtr]::Zero) { $p = Get-Process SmartRAM -ErrorAction SilentlyContinue; $h = $p.MainWindowHandle }
if ($h -eq [IntPtr]::Zero) { Write-Host 'widget NOT found'; exit 1 }
$r = New-Object Win2+RECT
[Win2]::GetWindowRect($h, [ref]$r) | Out-Null

# move away first, then click CLEAN NOW (design centre 158,369 -> client +30 padding)
[Win2]::SetCursorPos($r.L + 188, $r.T + 399) | Out-Null
Start-Sleep -Milliseconds 150
[Win2]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 60
[Win2]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)

Start-Sleep -Milliseconds 350
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_cleaning.png" | Out-Null
Start-Sleep -Milliseconds 1400
powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out "$PSScriptRoot\shot_toast.png" | Out-Null
Write-Host 'clean test shots saved'
