# ============================================================================
#  make_icon.ps1 — generates res/app.ico and res/icon-preview.png with GDI+.
#
#  Design: three slider tracks on the suite's dark card - the universal
#  "settings" mark - with **GP** over them. The sliders are a watermark at 32 px
#  and up, where the letters carry the tile; at 16 and 24 px the letters would be
#  a smudge (the same arithmetic that forced two tiers in HardWin), so those two
#  frames draw the sliders alone, larger and brighter.
#
#  Sizes 16 / 24 / 32 / 48 / 64 are written as uncompressed 32-bit DIBs (the
#  form every Windows shell renders), 128 and 256 as PNG (what Explorer and the
#  taskbar pick for large icons). Run from anywhere:
#
#      powershell -ExecutionPolicy Bypass -File HomePolicy\tools\make_icon.ps1
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
$ink       = [System.Drawing.Color]::FromArgb(255, 248, 250, 255) # near-white letters
$accent    = [System.Drawing.Color]::FromArgb(255, 84, 158, 255)  # #549EFF one knob
$trackDim  = [System.Drawing.Color]::FromArgb(255, 58, 70, 94)    # watermark track
$knobDim   = [System.Drawing.Color]::FromArgb(255, 74, 92, 122)   # watermark knob
$trackBold = [System.Drawing.Color]::FromArgb(255, 96, 118, 158)  # small-size track
$knobBold  = [System.Drawing.Color]::FromArgb(255, 232, 240, 252) # small-size knob

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
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
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

    # ---- sliders. Below 32 px they are the whole mark: full width, thick, and
    # bright, because a watermark at 16 px is a dark square. At 32 px and up they
    # recede behind the letters.
    $bold = $size -lt 32
    if ($bold) {
        $trackWidth = 0.74 * $s
        $thickness = 0.148 * $s
        $knobSize = 0.200 * $s
        $trackColour = $trackBold
        $knobColour = $knobBold
        $ys = @(0.255, 0.500, 0.745)
        $knobs = @(0.36, 0.665, 0.44)
    } else {
        $trackWidth = 0.62 * $s
        $thickness = 0.042 * $s
        $knobSize = 0.095 * $s
        $trackColour = $trackDim
        $knobColour = $knobDim
        $ys = @(0.300, 0.500, 0.700)
        $knobs = @(0.38, 0.63, 0.46)
    }

    $trackBrush = New-Object System.Drawing.SolidBrush $trackColour
    $knobBrush = New-Object System.Drawing.SolidBrush $knobColour
    $accentBrush = New-Object System.Drawing.SolidBrush $accent
    $trackLeft = ($s - $trackWidth) / 2
    for ($i = 0; $i -lt 3; $i++) {
        $centreY = $ys[$i] * $s
        $bar = New-RoundedPath $trackLeft ($centreY - $thickness / 2) $trackWidth $thickness ($thickness / 2)
        $g.FillPath($trackBrush, $bar)
        $bar.Dispose()
        $knobX = $trackLeft + $knobs[$i] * $trackWidth - $knobSize / 2
        $brush = $knobBrush
        if ($bold -and $i -eq 1) { $brush = $accentBrush }
        $g.FillEllipse($brush, $knobX, $centreY - $knobSize / 2, $knobSize, $knobSize)
    }
    $trackBrush.Dispose()
    $knobBrush.Dispose()
    $accentBrush.Dispose()

    if (-not $bold) {
        # ---- the letters, fitted by measurement so both fit inside the corners
        $fontSize = 0.42 * $s
        for ($attempt = 0; $attempt -lt 8; $attempt++) {
            $font = New-Object System.Drawing.Font 'Segoe UI', ([single]$fontSize), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
            $extent = $g.MeasureString('GP', $font)
            $font.Dispose()
            if ($extent.Width -le 0.001) { break }
            $fontSize = $fontSize * ((0.56 * $s) / $extent.Width)
            if ([Math]::Abs($extent.Width - (0.56 * $s)) -lt 0.4) { break }
        }
        $font = New-Object System.Drawing.Font 'Segoe UI', ([single]$fontSize), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
        $extent = $g.MeasureString('GP', $font)
        $shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 12, 14, 20))
        $inkBrush = New-Object System.Drawing.SolidBrush $ink
        $off = [single]([Math]::Max(0.5, 0.014 * $s))
        $x = ($s - $extent.Width) / 2
        $y = (0.50 * $s) - ($extent.Height / 2)
        $g.DrawString('GP', $font, $shadow, $x + $off, $y + $off)
        $g.DrawString('GP', $font, $inkBrush, $x, $y)
        $shadow.Dispose()
        $inkBrush.Dispose()
        $font.Dispose()
    }

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
