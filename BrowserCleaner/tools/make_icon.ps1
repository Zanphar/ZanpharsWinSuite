# make_icon.ps1 - generates BrowserCleaner.ico (16..256 px, PNG-compressed)
# Design: dark rounded square + blue gradient shield with a white check mark,
# matching the application's dark/cleaner theme.
Add-Type -AssemblyName System.Drawing

Function New-IconBitmap([int]$size) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $s = $size / 256.0

    # rounded-square backdrop (dark, like the app window)
    $bgBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 22, 25, 30))
    $bgPen   = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 52, 57, 67), [float](4 * $s))
    $bgPath  = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $r = 52 * $s
    $bg = [System.Drawing.RectangleF]::new([float](2*$s), [float](2*$s), [float]($size - 4*$s), [float]($size - 4*$s))
    $bgPath.AddArc($bg.Left,  $bg.Top,    2*$r, 2*$r, 180, 90)
    $bgPath.AddArc($bg.Right - 2*$r, $bg.Top,    2*$r, 2*$r, 270, 90)
    $bgPath.AddArc($bg.Right - 2*$r, $bg.Bottom - 2*$r, 2*$r, 2*$r,   0, 90)
    $bgPath.AddArc($bg.Left,  $bg.Bottom - 2*$r, 2*$r, 2*$r,  90, 90)
    $bgPath.CloseFigure()
    $gfx.FillPath($bgBrush, $bgPath)
    $gfx.DrawPath($bgPen, $bgPath)

    # shield (blue gradient) with white check
    $shield = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new([float](128*$s), [float](36*$s)),
        [System.Drawing.PointF]::new([float](218*$s), [float](64*$s)),
        [System.Drawing.PointF]::new([float](218*$s), [float](134*$s)),
        [System.Drawing.PointF]::new([float](128*$s), [float](222*$s)),
        [System.Drawing.PointF]::new([float](38*$s),  [float](134*$s)),
        [System.Drawing.PointF]::new([float](38*$s),  [float](64*$s))
    )
    $shPath = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $shPath.AddPolygon($shield)
    $grad = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
                $bg,  # bounding box drives the gradient
                [System.Drawing.Color]::FromArgb(255, 66, 133, 244),
                [System.Drawing.Color]::FromArgb(255, 29, 78, 216),
                65.0)
    $gfx.FillPath($grad, $shPath)
    $rim = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 235, 244, 255), [float](2.5 * $s))
    $gfx.DrawPath($rim, $shPath)

    # white check mark
    $check = [System.Drawing.Pen]::new([System.Drawing.Color]::White, [float](20 * $s))
    $check.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $check.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $gfx.DrawLine($check, (72*$s), (126*$s), (110*$s), (166*$s))
    $gfx.DrawLine($check, (110*$s), (166*$s), (186*$s), (92*$s))

    $gfx.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$images = @()
foreach ($sz in $sizes) {
    $bmp = New-IconBitmap $sz
    $ms = [System.IO.MemoryStream]::new()
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($sz -eq 256) { [System.IO.File]::WriteAllBytes("$PSScriptRoot\..\src\icon_preview.png", $ms.ToArray()) }
    $bmp.Dispose()
    $images += , @{ Size = $sz; Png = $ms.ToArray() }
    $ms.Dispose()
}

# pack into .ico (PNG-compressed entries are valid since Windows Vista)
$out = [System.IO.MemoryStream]::new()
$bw  = [System.IO.BinaryWriter]::new($out)
$bw.Write([uint16]0)     # reserved
$bw.Write([uint16]1)     # type: icon
$bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $sz  = $img.Size
    $dim = if ($sz -ge 256) { 0 } else { $sz }
    $bw.Write([byte]$dim)     # width
    $bw.Write([byte]$dim)     # height
    $bw.Write([byte]0)        # palette
    $bw.Write([byte]0)        # reserved
    $bw.Write([uint16]1)      # color planes
    $bw.Write([uint16]32)     # bits per pixel
    $bw.Write([uint32]$img.Png.Length)
    $bw.Write([uint32]$offset)
    $offset += $img.Png.Length
}
foreach ($img in $images) { $bw.Write($img.Png) }
$bw.Flush()
[System.IO.File]::WriteAllBytes("$PSScriptRoot\..\src\BrowserCleaner.ico", $out.ToArray())
$bw.Dispose()
$out.Dispose()
Write-Host 'BrowserCleaner.ico written:' $images.Count 'sizes'
