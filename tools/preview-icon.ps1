# Renders src/app.ico at several sizes on light and dark backgrounds so the icon can
# be eyeballed the way Explorer, the taskbar and Alt+Tab will show it.
# Writes shots/icon-sizes.png.
#   powershell -ExecutionPolicy Bypass -File tools/preview-icon.ps1
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$icoPath = Join-Path $root 'src\app.ico'
$shots = Join-Path $root 'shots'
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$out = Join-Path $shots 'icon-sizes.png'

$bmp = New-Object System.Drawing.Bitmap 900, 780
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(255, 246, 247, 249))
$dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 32, 34, 38))
$g.FillRectangle($dark, 0, 390, 900, 390)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$label = New-Object System.Drawing.Font 'Segoe UI', 11

$x = 12
foreach ($s in @(256, 48, 32, 16)) {
    $icon = New-Object System.Drawing.Icon $icoPath, $s, $s
    $ib = $icon.ToBitmap()
    # 256 is shown 1:1; the small sizes are magnified 4x so their pixels are visible.
    $draw = if ($s -eq 256) { 256 } else { $s * 4 }
    $g.DrawString(("{0}px" -f $s), $label, [System.Drawing.Brushes]::Gray, $x, 14)
    $g.DrawImage($ib, $x, 60, $draw, $draw)
    $g.DrawImage($ib, $x, 430, $draw, $draw)
    $ib.Dispose()
    $icon.Dispose()
    $x += $draw + 22
}

$label.Dispose()
$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"wrote {0}" -f $out
