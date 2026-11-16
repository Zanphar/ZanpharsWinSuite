# Builds src/app.ico from scratch: the same mark the app draws in its sidebar,
# a rounded accent-green tile with a dark Z.
#
#   powershell -ExecutionPolicy Bypass -File tools/make-icon.ps1
#
# Every size is drawn at its own resolution (never downscaled from 256) so the
# small frames stay crisp in Explorer, the taskbar and Alt+Tab.

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$outIcon = Join-Path $root 'src\app.ico'
$frameDir = Join-Path $PSScriptRoot 'frames'
New-Item -ItemType Directory -Path $frameDir -Force | Out-Null

$accentTop = [System.Drawing.Color]::FromArgb(255, 92, 236, 158)    # theme accent, lifted
$accentBottom = [System.Drawing.Color]::FromArgb(255, 40, 190, 115) # theme accent, sunk
$ink = [System.Drawing.Color]::FromArgb(255, 9, 26, 18)             # theme sidebar ink
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)

function New-RoundedPath([System.Drawing.RectangleF]$r, [float]$radius) {
    $d = $radius * 2
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($r.X, $r.Y, $d, $d, 180, 90)
    $p.AddArc($r.Right - $d, $r.Y, $d, $d, 270, 90)
    $p.AddArc($r.Right - $d, $r.Bottom - $d, $d, $d, 0, 90)
    $p.AddArc($r.X, $r.Bottom - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size,
        ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Tile: a Windows 11 style squircle sits at roughly 22-24% corner radius.
    $pad = [float]([math]::Max(0.5, $size * 0.04))
    $rect = New-Object System.Drawing.RectangleF $pad, $pad,
        ($size - 2 * $pad), ($size - 2 * $pad)
    $tile = New-RoundedPath $rect ([float]($size * 0.235))

    $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.Rectangle 0, 0, $size, $size)), $accentTop, $accentBottom,
        ([System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillPath($gradient, $tile)
    $gradient.Dispose()

    # The Z is built as a glyph outline so it can be measured and centred by its
    # actual ink, which keeps it optically centred at every size.
    $family = New-Object System.Drawing.FontFamily 'Segoe UI'
    $em = [float]($size * 0.78)
    $glyph = New-Object System.Drawing.Drawing2D.GraphicsPath
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Near
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Near
    $glyph.AddString('Z', $family, [int][System.Drawing.FontStyle]::Bold, $em,
        (New-Object System.Drawing.PointF 0, 0), $fmt)
    $bounds = $glyph.GetBounds()
    $fmt.Dispose()

    # Scale cap height to ~52% of the tile, then centre on the tile, not the canvas,
    # so the mark keeps its margin even though the tile is inset by $pad.
    $targetH = [float]($size * 0.52)
    $scale = $targetH / $bounds.Height
    $dx = [float]($rect.X + ($rect.Width - $bounds.Width * $scale) / 2 - $bounds.X * $scale)
    $dy = [float]($rect.Y + ($rect.Height - $bounds.Height * $scale) / 2 - $bounds.Y * $scale)
    $matrix = New-Object System.Drawing.Drawing2D.Matrix
    $matrix.Scale($scale, $scale)
    $matrix.Translate($dx, $dy, [System.Drawing.Drawing2D.MatrixOrder]::Prepend)
    $glyph.Transform($matrix)
    $matrix.Dispose()

    $inkBrush = New-Object System.Drawing.SolidBrush $ink
    $g.FillPath($inkBrush, $glyph)
    $inkBrush.Dispose()
    $glyph.Dispose()
    $family.Dispose()
    $tile.Dispose()
    $g.Dispose()

    $file = Join-Path $frameDir ("frame-{0}.png" -f $size)
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $file
}

$frames = @()
foreach ($size in $sizes) { $frames += New-Frame $size }

# ICO frames: uncompressed DIB for the small sizes (what .NET and older shell code
# expect), PNG for 256 (only Vista+ needs it, and a DIB that size would be ~270 KB).
function Get-DibBytes([string]$pngPath, [int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $pngPath
    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $locked = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $raw = New-Object byte[] ($locked.Stride * $size)
    [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $raw, 0, $raw.Length)
    $bmp.UnlockBits($locked)
    $bmp.Dispose()

    $maskStride = [int]([math]::Floor(($size + 31) / 32) * 4)
    $imageSize = $size * $size * 4 + $maskStride * $size

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms
    $bw.Write([UInt32]40)                 # biSize
    $bw.Write([Int32]$size)               # biWidth
    $bw.Write([Int32]($size * 2))         # biHeight: XOR + AND
    $bw.Write([UInt16]1)                  # biPlanes
    $bw.Write([UInt16]32)                 # biBitCount
    $bw.Write([UInt32]0)                  # biCompression: BI_RGB
    $bw.Write([UInt32]$imageSize)
    $bw.Write([Int32]0); $bw.Write([Int32]0)
    $bw.Write([UInt32]0); $bw.Write([UInt32]0)
    for ($y = $size - 1; $y -ge 0; $y--) { $bw.Write($raw, $y * $locked.Stride, $size * 4) }
    $bw.Write((New-Object byte[] ($maskStride * $size)), 0, $maskStride * $size)
    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

# ICO container.
$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $stream
$writer.Write([UInt16]0)               # reserved
$writer.Write([UInt16]1)               # type: icon
$writer.Write([UInt16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
$payloads = @()
foreach ($file in $frames) {
    $dim = [int]([System.IO.Path]::GetFileNameWithoutExtension($file).Split('-')[1])
    if ($dim -ge 256) {
        $bytes = [System.IO.File]::ReadAllBytes($file)
    } else {
        $bytes = Get-DibBytes $file $dim
    }
    $payloads += ,$bytes
    $value = if ($dim -ge 256) { [byte]0 } else { [byte]$dim }
    $writer.Write($value)              # width
    $writer.Write($value)              # height
    $writer.Write([byte]0)             # palette
    $writer.Write([byte]0)             # reserved
    $writer.Write([UInt16]1)           # planes
    $writer.Write([UInt16]32)          # bits per pixel
    $writer.Write([UInt32]$bytes.Length)
    $writer.Write([UInt32]$offset)
    $offset += $bytes.Length
}
foreach ($bytes in $payloads) { $writer.Write($bytes) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($outIcon, $stream.ToArray())
$writer.Dispose()
$stream.Dispose()

Remove-Item $frameDir -Recurse -Force
"wrote {0} ({1} bytes, {2} sizes)" -f $outIcon, (Get-Item $outIcon).Length, $sizes.Count
