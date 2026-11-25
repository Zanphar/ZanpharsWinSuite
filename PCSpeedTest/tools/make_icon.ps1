# ============================================================================
#  make_icon.ps1 — generates res/app.ico and res/icon-preview.png with GDI+.
#
#  Design: a speedometer in the suite palette — a semicircular arc, a needle
#  pointing up and to the right, and a hub — on the dark card every other
#  program uses, so the tile reads as "speed" at 16 px and at 256 px.
#
#  Sizes 16 / 24 / 32 / 48 / 64 are written as uncompressed 32-bit DIBs (the
#  form every Windows shell renders), 128 and 256 as PNG (what Explorer and the
#  taskbar pick for large icons). Run from anywhere:
#
#      powershell -ExecutionPolicy Bypass -File PCSpeedTest\tools\make_icon.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$resDir = [System.IO.Path]::GetFullPath($resDir)
$icoPath = Join-Path $resDir 'app.ico'
$previewPath = Join-Path $resDir 'icon-preview.png'

# ----------------------------------------------------------------- palette
$bg      = [System.Drawing.Color]::FromArgb(255, 34, 36, 44)     # #22242C card
$stroke  = [System.Drawing.Color]::FromArgb(255, 74, 80, 96)     # #4A5060
$arc     = [System.Drawing.Color]::FromArgb(255, 96, 141, 214)   # #608DD6 dial
$needle  = [System.Drawing.Color]::FromArgb(255, 84, 158, 255)   # #549EFF pointer
$ink     = [System.Drawing.Color]::FromArgb(255, 248, 250, 255)  # near-white

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

    # ---- tile: the dark card the suite uses everywhere
    $tile = New-RoundedPath (0.015 * $s) (0.015 * $s) (0.97 * $s) (0.97 * $s) (0.22 * $s)
    $bgBrush = New-Object System.Drawing.SolidBrush $bg
    $g.FillPath($bgBrush, $tile)
    $edge = New-Object System.Drawing.Pen $stroke, ([Math]::Max(1.0, 0.012 * $s))
    $g.DrawPath($edge, $tile)
    $bgBrush.Dispose()
    $edge.Dispose()
    $tile.Dispose()

    # ---- dial: a 180-degree sweep with a gap at the bottom for the hub, so the
    # shape reads as a gauge rather than a circle at 16 px.
    $pen = New-Object System.Drawing.Pen $arc, (0.105 * $s)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($pen, (0.20 * $s), (0.30 * $s), (0.60 * $s), (0.60 * $s), 180, 180)
    $pen.Dispose()

    # ---- needle: up and to the right, stopping short of the dial
    $hubX = [single](0.50 * $s)
    $hubY = [single](0.60 * $s)
    $needlePen = New-Object System.Drawing.Pen $needle, (0.09 * $s)
    $needlePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $needlePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($needlePen, $hubX, $hubY, [single](0.70 * $s), [single](0.34 * $s))
    $needlePen.Dispose()

    # ---- hub
    $hubBrush = New-Object System.Drawing.SolidBrush $ink
    $g.FillEllipse($hubBrush, ($hubX - 0.075 * $s), ($hubY - 0.075 * $s), (0.15 * $s), (0.15 * $s))
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
    # BITMAPINFOHEADER, height doubled: XOR image plus AND mask
    $bw.Write([int]40); $bw.Write([int]$w); $bw.Write([int]($h * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($w * $h * 4)); $bw.Write([int]0); $bw.Write([int]0)
    $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $h - 1; $y -ge 0; $y--) {
        $bw.Write($raw, $y * $stride, $w * 4)
    }
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
    if ($entry.Size -eq 256) {
        $bmp.Save($previewPath, [System.Drawing.Imaging.ImageFormat]::Png)
    }
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
