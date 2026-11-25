# ============================================================================
#  make_icon.ps1 — generates res/app.ico and res/icon-preview.png with GDI+.
#
#  Design: a padlock sits behind the name as the backdrop — body and shackle
#  in dim slate — with "Hardened" over "Windows" set in front of it, so
#  the tile reads as the product name over a lock at 32 px and up.
#
#  At 16 and 24 px - the frames the title bar picks - the name gives way to a
#  tight "HW" on one line, drawn as glyph outlines at the largest size the tile
#  takes. Two stacked rows split fifteen pixels into five each and the letters
#  crowded each other; one row keeps them apart and keeps W (the wider letter)
#  inside the corners. The lock is dropped there: at this size it is a dim shape
#  competing with the letters for the same handful of pixels.
#
#  Sizes 16 / 24 / 32 / 48 / 64 are written as uncompressed 32-bit DIBs (the
#  form every Windows shell renders), 128 and 256 as PNG (what Explorer and the
#  taskbar pick for large icons). Run from anywhere:
#
#      powershell -ExecutionPolicy Bypass -File HardWin\tools\make_icon.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resDir = Join-Path $PSScriptRoot '..\res'
$resDir = [System.IO.Path]::GetFullPath($resDir)
$icoPath = Join-Path $resDir 'app.ico'
$previewPath = Join-Path $resDir 'icon-preview.png'

# ----------------------------------------------------------------- palette
$bg       = [System.Drawing.Color]::FromArgb(255, 34, 36, 44)     # #22242C card
$stroke   = [System.Drawing.Color]::FromArgb(255, 74, 80, 96)     # #4A5060
# The lock is a watermark, not a second subject: two dim slate tones that sit a
# step above the card. Anything brighter competes with the initials at 16 px and
# the tile turns into a blue smudge.
$lockBody = [System.Drawing.Color]::FromArgb(255, 44, 56, 78)     # #2C384E padlock body
$lockEdge = [System.Drawing.Color]::FromArgb(255, 62, 84, 118)    # #3E5476 shackle and rim
$ink      = [System.Drawing.Color]::FromArgb(255, 248, 250, 255)  # near-white

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

# Two initials have to survive five pixels of cap height, so they are drawn as
# font outlines and placed by their own bounds. Hinted text at 5 px fuses its
# strokes, and a placement measured from the string box leaves no gap between
# the rows: the outline's bounds are the letter itself.
# Bold, not the lighter weights: at five pixels of cap height a semibold stroke
# lands under a pixel wide and antialiases to grey, which is exactly the smudge
# this size is trying to avoid. Bold fills the stroke; the two letters stay
# apart because their shapes are far apart, not because the strokes are thin.
$initialFamily = 'Segoe UI'
$initialStyle = [int][System.Drawing.FontStyle]::Bold

function New-InitialPath([string]$letter, [single]$capHeight) {
    $family = New-Object System.Drawing.FontFamily $initialFamily
    $format = [System.Drawing.StringFormat]::GenericTypographic
    $origin = New-Object System.Drawing.PointF 0, 0

    $probe = New-Object System.Drawing.Drawing2D.GraphicsPath
    $probe.AddString($letter, $family, $initialStyle, [single]64,
                     $origin, $format)
    $bounds = $probe.GetBounds()
    $probe.Dispose()

    # Same outline, set at the em that gives it exactly the requested cap height.
    $em = [single](64 * ($capHeight / $bounds.Height))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString($letter, $family, $initialStyle, $em, $origin, $format)
    return $path
}

# Fills a glyph outline at a whole-pixel position: a letter half a pixel off the
# grid is drawn as two grey half-strokes instead of one white one, which at 16 px
# is the difference between a letter and a smudge.
function Draw-Path($g, $path, [single]$left, [single]$top, $brush) {
    $bounds = $path.GetBounds()
    $matrix = New-Object System.Drawing.Drawing2D.Matrix
    $matrix.Translate(($left - $bounds.X), ($top - $bounds.Y))
    $path.Transform($matrix)
    $matrix.Dispose()
    $g.FillPath($brush, $path)
}

function New-IconBitmap([int]$size) {
    $s = [single]$size
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
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

    # ---- 16 and 24 px: a tight "HW" on one line, and nothing else. Stacking H
    # over W split fifteen pixels into two rows of five, so the two letters
    # crowded each other as much as the tile crowded them; one row gives each
    # letter the full height and a clear gap beside it. The lock is dropped here:
    # at this size a watermark is a dim shape competing with the letters for the
    # same handful of pixels.
    if ($size -lt 32) {
        $capHeight = 0.38 * $s
        $gap = [single]([Math]::Max(1.0, 0.05 * $s))
        $h = New-InitialPath 'H' $capHeight
        $w = New-InitialPath 'W' $capHeight

        # Glyph widths scale with the cap height, so one measurement is enough to
        # keep the pair inside the card's rounded corners.
        $total = $h.GetBounds().Width + $gap + $w.GetBounds().Width
        if ($total -gt 0.80 * $s) {
            $capHeight = $capHeight * ((0.80 * $s) / $total)
            $h.Dispose(); $w.Dispose()
            $h = New-InitialPath 'H' $capHeight
            $w = New-InitialPath 'W' $capHeight
            $total = $h.GetBounds().Width + $gap + $w.GetBounds().Width
        }

        $left = [single]([Math]::Round(($s - $total) / 2))
        $top = [single]([Math]::Round(($s - $capHeight) / 2))
        $inkBrush = New-Object System.Drawing.SolidBrush $ink
        $hWidth = $h.GetBounds().Width
        Draw-Path $g $h $left $top $inkBrush
        Draw-Path $g $w ([single]([Math]::Round($left + $hWidth + $gap))) $top $inkBrush
        $inkBrush.Dispose()
        $h.Dispose(); $w.Dispose()
        $g.Dispose()
        return $bmp
    }

    # ---- padlock: shackle first, then the body over its ends. It is a dim
    # backdrop behind the name - the words are the mark, the lock is what they
    # stand for - and it is drawn only here, from 32 px up.
    $shackleRect = @((0.295 * $s), (0.17 * $s), (0.41 * $s), (0.40 * $s))
    $shackleWidth = 0.098 * $s
    $bodyRect = @((0.175 * $s), (0.40 * $s), (0.65 * $s), (0.45 * $s))
    $bodyRadius = 0.075 * $s
    $bodyColor = $lockBody
    $edgeColor = $lockEdge

    $shacklePen = New-Object System.Drawing.Pen $edgeColor, $shackleWidth
    $shacklePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $shacklePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($shacklePen, $shackleRect[0], $shackleRect[1], $shackleRect[2], $shackleRect[3], 180, 180)
    $shacklePen.Dispose()

    $body = New-RoundedPath $bodyRect[0] $bodyRect[1] $bodyRect[2] $bodyRect[3] $bodyRadius
    $lockBrush = New-Object System.Drawing.SolidBrush $bodyColor
    $g.FillPath($lockBrush, $body)
    $lockBrush.Dispose()
    $rim = New-Object System.Drawing.Pen $edgeColor, ([Math]::Max(1.0, 0.014 * $s))
    $g.DrawPath($rim, $body)
    $rim.Dispose()
    $body.Dispose()

    # The keyhole is detail, and detail below 48 px is dirt: it is drawn only
    # where there are pixels to spend on it, in the card colour so it stays quiet.
    if ($size -ge 48) {
        $holeBrush = New-Object System.Drawing.SolidBrush $bg
        $hole = New-RoundedPath (0.377 * $s) (0.53 * $s) (0.046 * $s) (0.13 * $s) (0.023 * $s)
        $g.FillPath($holeBrush, $hole)
        $g.FillEllipse($holeBrush, (0.355 * $s), (0.478 * $s), (0.09 * $s), (0.09 * $s))
        $hole.Dispose()
        $holeBrush.Dispose()
    }

    # ---- the name: "Hardened" over "Windows"
    # Each word is measured and scaled to the same width rather than set at a
    # guessed point size: "Hardened" is the longer word, and both lines have to
    # end at the same edge or the tile looks tilted. Measured placement also means
    # neither line can overflow the rounded corners.
    function Get-FittedFont([string]$text, [single]$targetWidth, [single]$startSize) {
        $fontSize = $startSize
        for ($attempt = 0; $attempt -lt 8; $attempt++) {
            $font = New-Object System.Drawing.Font 'Segoe UI', ([single]$fontSize), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
            $extent = $g.MeasureString($text, $font)
            $font.Dispose()
            if ($extent.Width -le 0.001) { break }
            $fontSize = $fontSize * ($targetWidth / $extent.Width)
            if ([Math]::Abs($extent.Width - $targetWidth) -lt 0.4) { break }
        }
        return (New-Object System.Drawing.Font 'Segoe UI', ([single]$fontSize), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel))
    }

    # "Hardened" over "Windows" from 32 px up, where eight characters a line can
    # still be read.
    $lineWidth = 0.72 * $s
    $topFont = Get-FittedFont 'Hardened' $lineWidth (0.30 * $s)
    $bottomFont = Get-FittedFont 'Windows' $lineWidth (0.30 * $s)

    $shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 12, 14, 20))
    $inkBrush = New-Object System.Drawing.SolidBrush $ink
    $off = [single]([Math]::Max(0.5, 0.012 * $s))
    # Two rows across the tile, upper and lower, so the pair reads as a name
    # rather than a word wrapped wherever the box ended.
    $lines = @(
        @('Hardened', $topFont, (0.315 * $s)),
        @('Windows', $bottomFont, (0.645 * $s))
    )

    # The comma keeps each pair a pair: a plain @() of two pairs is flattened,
    # and DrawString then gets a brush where it wants a string.
    $passes = @()
    $passes += ,@($shadow, $off)
    $passes += ,@($inkBrush, [single]0)
    foreach ($pass in $passes) {
        $brush = $pass[0]
        $shift = [single]$pass[1]
        foreach ($line in $lines) {
            # Not $size: that name is the function's typed [int] parameter.
            $measured = $g.MeasureString($line[0], $line[1])
            $x = ($s - $measured.Width) / 2 + $shift
            $y = [single]$line[2] - ($measured.Height / 2) + $shift
            $g.DrawString($line[0], $line[1], $brush, $x, $y)
        }
    }

    $inkBrush.Dispose()
    $shadow.Dispose()
    $topFont.Dispose()
    $bottomFont.Dispose()

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
    # pixels, bottom-up
    for ($y = $h - 1; $y -ge 0; $y--) {
        $bw.Write($raw, $y * $stride, $w * 4)
    }
    # AND mask: every bit zero, rows padded to 4 bytes
    $maskRow = [int](([Math]::Floor(($w + 31) / 32)) * 4)
    $zeros = New-Object byte[] $maskRow
    for ($y = 0; $y -lt $h; $y++) { $bw.Write($zeros, 0, $maskRow) }
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    # The comma stops PowerShell unrolling the array into separate objects.
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
