# tools/make_icon.ps1 â€” generates res/app.ico (PNG-in-ICO frames).
Add-Type -AssemblyName System.Drawing

function New-Frame([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(4, [int]($size * 0.22))       # corner radius
    $gp = New-Object System.Drawing.Drawing2D.GraphicsPath
    $gp.AddArc(0, 0, 2*$r, 2*$r, 180, 90)
    $gp.AddArc($size - 2*$r, 0, 2*$r, 2*$r, 270, 90)
    $gp.AddArc($size - 2*$r, $size - 2*$r, 2*$r, 2*$r, 0, 90)
    $gp.AddArc(0, $size - 2*$r, 2*$r, 2*$r, 90, 90)
    $gp.CloseFigure()

    $gradRect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $gradRect,
        [System.Drawing.Color]::FromArgb(255, 37, 42, 54),
        [System.Drawing.Color]::FromArgb(255, 11, 14, 21),
        90)
    $g.FillPath($grad, $gp)

    # ring track + progress arc (72%)
    $inset = [float]($size * 0.26)
    $ringRect = New-Object System.Drawing.RectangleF($inset, $inset, ($size - 2*$inset), ($size - 2*$inset))
    $ringBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $gradRect,
        [System.Drawing.Color]::FromArgb(255, 34, 211, 238),
        [System.Drawing.Color]::FromArgb(255, 52, 211, 153),
        60)
    $pen = New-Object System.Drawing.Pen($ringBrush, ($size * 0.085))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawEllipse($pen, $ringRect)
    $g.DrawArc($pen, $ringRect, -90, 259)

    # bolt glyph (white)
    $bolt = New-Object 'System.Drawing.PointF[]' 6
    $def = @(
        @( 0.045, -0.225), @(-0.105, 0.035), @(-0.020, 0.035),
        @(-0.060, 0.225), @( 0.115, -0.030), @( 0.030, -0.030))
    for ($i = 0; $i -lt 6; $i++) {
        $bolt[$i] = New-Object System.Drawing.PointF(
            [float]($size/2 + $def[$i][0] * $size * 0.9),
            [float]($size/2 + $def[$i][1] * $size * 0.9))
    }
    $white = [System.Drawing.Brushes]::White
    $g.FillPolygon($white, $bolt)

    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $ms.ToArray()
}

$sizes = @(256, 128, 64, 48, 32, 16)
$frames = @{}
$imgDir = @()
$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $png = New-Frame $s
    $frames[$s] = $png
    $imgDir += @{ size = $s; len = $png.Length }
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
foreach ($d in $imgDir) {
    Write-Host ("frame " + $d.size + " len " + $d.len)
    $b = if ($d.size -ge 256) { 0 } else { $d.size }
    $w.Write([byte]$b); $w.Write([byte]$b)
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$d.len)
    $w.Write([UInt32]$offset)
    $offset += $d.len
}
foreach ($s in $sizes) { $w.Write([byte[]]$frames[$s]) }
$w.Flush()
[System.IO.File]::WriteAllBytes("$PSScriptRoot\..\res\app.ico", $out.ToArray())
Write-Host "app.ico written: $($out.Length) bytes"


