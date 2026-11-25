# ============================================================================
#  make_icon.ps1 — generates res/app.ico and res/icon-preview.png with GDI+.
#
#  Design: a speedometer on the suite's dark card - the tuning mark. A thick
#  accent arc runs 240 degrees and a bright needle points up and to the right
#  (fast), with an accent hub. At 16 and 24 px everything thickens and the tick
#  dots are dropped, because a hairline arc disappears at that size.
#
#  Sizes 16 / 24 / 32 / 48 / 64 are uncompressed 32-bit DIBs, 128 and 256 are
#  PNG. Run from anywhere:
#
#      powershell -ExecutionPolicy Bypass -File InternetTuningWizard\tools\make_icon.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$resDir = [System.IO.Path]::GetFullPath($resDir)
$icoPath = Join-Path $resDir 'app.ico'
$previewPath = Join-Path $resDir 'icon-preview.png'

# ----------------------------------------------------------------- palette
$bg        = [System.Drawing.Color]::FromArgb(255, 34, 36, 44)    # #22242C card
$stroke    = [System.Drawing.Color]::FromArgb(255, 74, 80, 96)    # #4A5060 rim
$ink       = [System.Drawing.Color]::FromArgb(255, 248, 250, 255) # needle
$accent    = [System.Drawing.Color]::FromArgb(255, 84, 158, 255)  # #549EFF arc
$arcDim    = [System.Drawing.Color]::FromArgb(255, 96, 118, 158)  # watermark arc
$tick      = [System.Drawing.Color]::FromArgb(255, 146, 152, 164) # ticks

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap([int]$size) {
    $s = [single]$size
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # ---- tile
    $tile = New-RoundedPath (0.015 * $s) (0.015 * $s) (0.97 * $s) (0.97 * $s) (0.22 * $s)
    $bgBrush = New-Object System.Drawing.SolidBrush $bg
    $g.FillPath($bgBrush, $tile)
    $edge = New-Object System.Drawing.Pen $stroke, ([Math]::Max(1.0, 0.012 * $s))
    $g.DrawPath($edge, $tile)
    $bgBrush.Dispose()
    $edge.Dispose()
    $tile.Dispose()

    # ---- gauge: 240 degrees, opening at the bottom
    $bold = $size -lt 32
    $radius = 0.30 * $s
    $cx = 0.5 * $s
    $cy = 0.56 * $s
    $rect = New-Object System.Drawing.RectangleF (($cx - $radius), ($cy - $radius), (2 * $radius), (2 * $radius))
    $thickness = if ($bold) { 0.155 * $s } else { 0.095 * $s }
    $arcPen = New-Object System.Drawing.Pen $accent, $thickness
    $arcPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $arcPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($arcPen, $rect, 150, 240)
    $arcPen.Dispose()

    # ---- ticks at the two ends and the top, dropped when tiny
    if (-not $bold) {
        $tickBrush = New-Object System.Drawing.SolidBrush $tick
        foreach ($angle in @(150, 270, 390)) {
            $rad = $angle * [Math]::PI / 180
            $tx = $cx + ([Math]::Cos($rad) * ($radius + 0.045 * $s))
            $ty = $cy + ([Math]::Sin($rad) * ($radius + 0.045 * $s))
            $ts = 0.035 * $s
            $g.FillEllipse($tickBrush, $tx - $ts, $ty - $ts, 2 * $ts, 2 * $ts)
        }
        $tickBrush.Dispose()
    }

    # ---- needle, pointing up and to the right
    $needleAngle = -55 * [Math]::PI / 180
    $needleLength = if ($bold) { 0.33 * $s } else { 0.27 * $s }
    $needlePen = New-Object System.Drawing.Pen $ink, $(if ($bold) { 0.115 * $s } else { 0.062 * $s })
    $needlePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $needlePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $ex = $cx + ([Math]::Cos($needleAngle) * $needleLength)
    $ey = $cy + ([Math]::Sin($needleAngle) * $needleLength)
    $g.DrawLine($needlePen, $cx, $cy, $ex, $ey)
    $needlePen.Dispose()

    # ---- hub
    $hubR = if ($bold) { 0.085 * $s } else { 0.052 * $s }
    $hubBrush = New-Object System.Drawing.SolidBrush $accent
    $g.FillEllipse($hubBrush, $cx - $hubR, $cy - $hubR, 2 * $hubR, 2 * $hubR)
    $hubBrush.Dispose()

    $g.Dispose()
    return $bmp
}

# ----------------------------------------------------------------- ICO writer
function Get-DibBytes([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $raw = New-Object byte[] ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($data)

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms
    $bw.Write([int]40); $bw.Write([int]$w); $bw.Write([int]($h * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($w * $h * 4)); $bw.Write([int]0); $bw.Write([int]0)
    $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $h - 1; $y -ge 0; $y--) { $bw.Write($raw, $y * $stride, $w * 4) }
    $maskRow = [int](([Math]::Floor(($w + 31) / 32)) * 4)
    $zeros = New-Object byte[] $maskRow
    for ($y = 0; $y -lt $h; $y++) { $bw.Write($zeros, 0, $maskRow) }
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

function Get-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return ,$bytes
}

$sizes = @(
    @{ Size = 16;  Png = $false },
    @{ Size = 24;  Png = $false },
    @{ Size = 32;  Png = $false },
    @{ Size = 48;  Png = $false },
    @{ Size = 64;  Png = $false },
    @{ Size = 128; Png = $true  },
    @{ Size = 256; Png = $true  }
)

$blobs = New-Object System.Collections.ArrayList
foreach ($entry in $sizes) {
    $bmp = New-IconBitmap $entry.Size
    if ($entry.Png) { $bytes = [byte[]](Get-PngBytes $bmp) }
    else            { $bytes = [byte[]](Get-DibBytes $bmp) }
    [void]$blobs.Add([pscustomobject]@{ Size = $entry.Size; Bytes = $bytes })
    if ($entry.Size -eq 256) { $bmp.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

$ico = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $ico
$writer.Write([int16]0); $writer.Write([int16]1); $writer.Write([int16]$blobs.Count)
$offset = 6 + (16 * $blobs.Count)
foreach ($blob in $blobs) {
    $dim = if ($blob.Size -ge 256) { 0 } else { $blob.Size }
    $writer.Write([byte]$dim); $writer.Write([byte]$dim)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([int16]1); $writer.Write([int16]32)
    $writer.Write([int]$blob.Bytes.Length); $writer.Write([int]$offset)
    $offset += $blob.Bytes.Length
}
foreach ($blob in $blobs) { $writer.Write($blob.Bytes, 0, $blob.Bytes.Length) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $ico.ToArray())
$writer.Dispose(); $ico.Dispose()

Write-Output ("wrote {0} ({1:N0} bytes)" -f $icoPath, (Get-Item $icoPath).Length)
Write-Output ("wrote {0}" -f $previewPath)
