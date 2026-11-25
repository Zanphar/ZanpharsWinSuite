# ============================================================================
#  make_icon.ps1 — generates res/app.ico and res/icon-preview.png with GDI+.
#
#  Design: the connection mark — a teal-to-blue gradient tile with signal arcs
#  radiating from the lower centre and an "ICU" monogram. Below 28 px the arcs
#  are dropped, because hairlines turn to noise at that size.
#
#  Sizes 16 / 24 / 32 / 48 / 64 are uncompressed 32-bit DIBs, 128 and 256 are
#  PNG. Run from anywhere:
#
#      powershell -ExecutionPolicy Bypass -File InternetConnectionUtility\tools\make_icon.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$resDir = [System.IO.Path]::GetFullPath($resDir)
$icoPath = Join-Path $resDir 'app.ico'
$previewPath = Join-Path $resDir 'icon-preview.png'

# ----------------------------------------------------------------- palette
$teal   = [System.Drawing.Color]::FromArgb(255, 0x23, 0xC9, 0xB7)
$blue   = [System.Drawing.Color]::FromArgb(255, 0x3F, 0x6F, 0xEA)
$ink    = [System.Drawing.Color]::FromArgb(255, 0xF8, 0xFA, 0xFF)
$rim    = [System.Drawing.Color]::FromArgb(46, 0x02, 0x10, 0x1A)
$markInk = [System.Drawing.Color]::FromArgb(150, 0x03, 0x1A, 0x28)

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
    $tile = New-RoundedPath (0.035 * $s) (0.035 * $s) (0.93 * $s) (0.93 * $s) (0.235 * $s)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.RectangleF 0, 0, $s, $s), $teal, $blue,
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)
    $g.FillPath($bgBrush, $tile)
    $edge = New-Object System.Drawing.Pen $rim, ([Math]::Max(1.0, 0.012 * $s))
    $g.DrawPath($edge, $tile)
    $bgBrush.Dispose()
    $edge.Dispose()

    # ---- gloss, clipped to the tile
    $g.SetClip($tile)
    $glossRect = New-Object System.Drawing.RectangleF (0.035 * $s), (0.035 * $s), (0.93 * $s), (0.51 * $s)
    $gloss = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        $glossRect,
        [System.Drawing.Color]::FromArgb(46, 255, 255, 255),
        [System.Drawing.Color]::FromArgb(0, 255, 255, 255),
        [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillRectangle($gloss, $glossRect)
    $gloss.Dispose()
    $g.ResetClip()
    $tile.Dispose()

    # ---- connection signal: concentric arcs opening upward
    if ($size -ge 28) {
        $cx = 0.5 * $s
        $cy = 0.94 * $s
        $radii = @(0.28, 0.40, 0.52)
        $alphas = @(125, 88, 55)
        for ($i = 0; $i -lt 3; $i++) {
            $r = $radii[$i] * $s
            $col = [System.Drawing.Color]::FromArgb($alphas[$i], 3, 26, 40)
            $pen = New-Object System.Drawing.Pen $col, ([Math]::Max(1.2, 0.045 * $s))
            $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
            $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
            $rect = New-Object System.Drawing.RectangleF (($cx - $r)), (($cy - $r)), (2 * $r), (2 * $r)
            $g.DrawArc($pen, $rect, 202, 136)
            $pen.Dispose()
        }
        $originBrush = New-Object System.Drawing.SolidBrush $markInk
        $o = 0.035 * $s
        $g.FillEllipse($originBrush, ($cx - $o), ($cy - $o), (2 * $o), (2 * $o))
        $originBrush.Dispose()
    }

    # ---- monogram
    $fscale = if ($size -le 20) { 0.34 } elseif ($size -le 40) { 0.38 } else { 0.42 }
    $family = New-Object System.Drawing.FontFamily "Segoe UI"
    $font = New-Object System.Drawing.Font $family, ($s * $fscale), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $textBrush = New-Object System.Drawing.SolidBrush $ink
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textRect = New-Object System.Drawing.RectangleF 0, (0.10 * $s), $s, (0.72 * $s)
    $g.DrawString("ICU", $font, $textBrush, $textRect, $fmt)
    $fmt.Dispose()
    $textBrush.Dispose()
    $font.Dispose()
    $family.Dispose()

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
