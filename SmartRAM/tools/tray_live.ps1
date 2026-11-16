# tray_live.ps1 — burst-capture OUR tray icon (UIA-located) during a RAM swing
# and during a clean; saves 6x-zoomed icon frames.
Add-Type -AssemblyName System.Drawing, UIAutomationClient, UIAutomationTypes

function Find-Ours {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Shell_TrayWnd')
    $bar = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
    $btnCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    $items = $bar.FindAll([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
    foreach ($it in $items) {
        if ($it.Current.Name -match '^Smart RAM RAM') { return $it.Current.BoundingRectangle }
    }
    return $null
}

function Snap-Icon([string]$tag, [double]$rx, [double]$ry) {
    $srcPath = "$PSScriptRoot\snap_full.png"
    powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\screenshot.ps1" -out $srcPath | Out-Null
    $src = [System.Drawing.Bitmap]::FromFile($srcPath)
    $x = [int]([math]::Floor($rx)) - 6; $y = [int]([math]::Floor($ry)) + 4
    $rect = New-Object System.Drawing.Rectangle($x, $y, 44, 44)
    if ($rect.X -lt 0) { $rect.X = 0 }
    $crop = $src.Clone($rect, $src.PixelFormat)
    $big = New-Object System.Drawing.Bitmap 264, 264
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($crop, 0, 0, 264, 264)
    $big.Save("$PSScriptRoot\icon_$tag.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $big.Dispose(); $src.Dispose(); $crop.Dispose()
}

$oursRect = Find-Ours
if (-not $oursRect) { Write-Host 'our tray icon NOT found'; exit 1 }
$ix = [double]$oursRect.X; $iy = [double]$oursRect.Y
Write-Host ("located at " + $ix + "," + $iy)

# --- phase 1: RAM swing (3 x 1 GB eater) ---
$job = Start-Job {
    $a = New-Object byte[] (1GB)
    $b = New-Object byte[] (1GB)
    $c = New-Object byte[] (1GB)
    for ($i = 0; $i -lt $a.Length; $i += 4096) { $a[$i] = 1; $b[$i] = 1; $c[$i] = 1 }
    Start-Sleep -Seconds 8
}
Start-Sleep -Milliseconds 1500
for ($i = 0; $i -lt 6; $i++) { Snap-Icon "swing_$i" $ix $iy; Start-Sleep -Milliseconds 350 }
Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue

# --- phase 2: spinner during a clean ---
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class M { 
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
'@
Add-Type -TypeDefinition $sig
$widget = Get-Process SmartRAM | Select-Object -First 1
# widget window at 1524,500 known coordinates; button centre +188/+399
$wx = 1524 + 188; $wy = 500 + 399
[M]::SetCursorPos($wx, $wy) | Out-Null
Start-Sleep -Milliseconds 150
[M]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 60
[M]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
for ($i = 0; $i -lt 4; $i++) { Snap-Icon "spin_$i" $ix $iy; Start-Sleep -Milliseconds 280 }
Write-Host 'icon frames saved'

