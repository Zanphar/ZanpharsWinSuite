# ============================================================================
#  make_icon.ps1 — generates resources/app.ico programmatically with GDI+.
#
#  Produces a modern flat shield glyph with a rounded check mark, packed as
#  a multi-size Windows icon (16/32/48 as BMP, 256 as PNG).
# ============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$OutPath = Join-Path $PSScriptRoot '..\resources\app.ico'

# ---------------------------------------------------------------- shield art

function New-ShieldBitmap([int]$size) {
    $bmp   = New-Object System.Drawing.Bitmap $size, $size
    $g     = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    # shield body (unit coordinates 0..1)
    $pts = @(
        (0.50, 0.04), (0.88, 0.14), (0.90, 0.46), (0.76, 0.76),
        (0.50, 0.94), (0.24, 0.76), (0.10, 0.46), (0.12, 0.14)
    ) | ForEach-Object {
        New-Object System.Drawing.PointF (($_[0] * $size)), (($_[1] * $size))
    }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddLines([System.Drawing.PointF[]]$pts)
    $path.CloseFigure()

    $from = [System.Drawing.Color]::FromArgb(255, 96, 165, 250)  # upper blue
    $to   = [System.Drawing.Color]::FromArgb(255, 56, 118, 224)  # lower blue
    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect, $from, $to, [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillPolygon($brush, [System.Drawing.PointF[]]$pts)

    # rounded check mark
    $penW    = [float]($size * 0.10)
    $pen     = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), $penW
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $check = @(
        (New-Object System.Drawing.PointF (($size * 0.32), ($size * 0.53))),
        (New-Object System.Drawing.PointF (($size * 0.45), ($size * 0.66))),
        (New-Object System.Drawing.PointF (($size * 0.72), ($size * 0.35)))
    )
    $g.DrawLines($pen, [System.Drawing.PointF[]]$check)

    $g.Dispose()
    return $bmp
}

# ---------------------------------------------------------------- ico packer

function Get-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return $ms.ToArray()
}

function Get-BmpBytes([System.Drawing.Bitmap]$bmp) {
    # BITMAPINFOHEADER + bottom-up 32bpp BGRA XOR data + empty AND mask
    $w = $bmp.Width; $h = $bmp.Height
    $rect  = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data  = $bmp.LockBits($rect,
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $plane  = $w * 4

    $out = New-Object System.IO.MemoryStream

    # BITMAPINFOHEADER
    $bw = New-Object System.IO.BinaryWriter($out)
    $bw.Write([uint32]40)            # biSize
    $bw.Write([int32]$w)             # biWidth
    $bw.Write([int32]($h * 2))       # biHeight (XOR + AND)
    $bw.Write([uint16]1)             # biPlanes
    $bw.Write([uint16]32)            # biBitCount
    $bw.Write([uint32]0)             # biCompression (BI_RGB)
    $bw.Write([uint32]($plane * $h)) # biSizeImage
    $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)

    # XOR data, bottom-up
    $xor = New-Object byte[] ($plane * $h)
    for ($y = 0; $y -lt $h; $y++) {
        $row = New-Object byte[] $plane
        [System.Runtime.InteropServices.Marshal]::Copy(
            [System.IntPtr]::Add($data.Scan0, $y * $stride), $row, 0, $plane)
        $dst = ($h - 1 - $y) * $plane
        [Array]::Copy($row, 0, $xor, $dst, $plane)
    }
    $bw.Write($xor)

    # AND mask: rows top-down, 1bpp, DWORD-aligned, all zero (alpha decides)
    $maskStride = [Math]::Ceiling($w / 8.0)
    $maskStride = ([int][Math]::Ceiling($maskStride / 4.0)) * 4
    $mask = New-Object byte[] ($maskStride * $h)
    $bw.Write($mask)

    $bmp.UnlockBits($data)
    $bw.Flush()
    return $out.ToArray()
}

function Write-Ico([string]$path) {
    $sizes = @(16, 32, 48, 256)
    $images = @()
    foreach ($s in $sizes) {
        $bmp = New-ShieldBitmap $s
        if ($s -ge 256) { $data = Get-PngBytes $bmp } else { $data = Get-BmpBytes $bmp }
        Write-Host "size $s -> $($data.Length) bytes"
        $images += @{ size = $s; data = $data }
        $bmp.Dispose()
    }

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([uint16]0)           # reserved
    $bw.Write([uint16]1)           # type: icon
    $bw.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($img in $images) {
        $s = $img.size
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))  # width (0 = 256)
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))  # height
        $bw.Write([byte]0)      # palette
        $bw.Write([byte]0)      # reserved
        $bw.Write([uint16]1)    # planes
        $bw.Write([uint16]32)   # bpp
        $bw.Write([uint32]$img.data.Length)
        $bw.Write([uint32]$offset)
        $offset += $img.data.Length
    }
    foreach ($img in $images) { $bw.Write([byte[]]$img.data) }
    $bw.Flush()
    [System.IO.File]::WriteAllBytes($path, $ms.ToArray())
    Write-Host "Wrote $path ($($ms.Length) bytes, sizes: $($sizes -join ', '))"
}

Write-Ico $OutPath
