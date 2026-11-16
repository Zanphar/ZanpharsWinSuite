# Verifies the icon actually ships: counts the icon resources inside the exe,
# extracts them the way the shell does, and reports what the desktop shortcut points
# its icon at. Writes shots/icon-from-exe.png for eyeballing.
#
#   powershell -ExecutionPolicy Bypass -File tools/check-icon.ps1
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class IconProbe {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconEx(string file, int index, IntPtr[] large, IntPtr[] small, uint count);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SHObjectProperties(IntPtr hwnd, uint flags, string path, string page);

    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    [DllImport("user32.dll")]
    public static extern bool DestroyIcon(IntPtr hIcon);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SHFILEINFO {
        public IntPtr hIcon;
        public int iIcon;
        public uint dwAttributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szDisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string szTypeName;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SHGetFileInfo(string path, uint attrs, ref SHFILEINFO info,
                                              uint cbInfo, uint flags);

    // Reads the RT_GROUP_ICON header so we can count the frames baked into the exe.
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryEx(string file, IntPtr hFile, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindResource(IntPtr hModule, IntPtr name, IntPtr type);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr hModule, IntPtr hRes);

    [DllImport("kernel32.dll")]
    public static extern IntPtr LockResource(IntPtr hResData);

    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr hModule);

    public static int GroupFrameCount(string file, int id) {
        IntPtr mod = LoadLibraryEx(file, IntPtr.Zero, 0x00000002);  // LOAD_LIBRARY_AS_DATAFILE
        if (mod == IntPtr.Zero) return -1;
        try {
            IntPtr res = FindResource(mod, (IntPtr)id, (IntPtr)14);      // RT_GROUP_ICON
            if (res == IntPtr.Zero) return 0;
            IntPtr data = LockResource(LoadResource(mod, res));
            if (data == IntPtr.Zero) return -1;
            return Marshal.ReadInt16(data, 4);                          // GRPICONDIR.idCount
        } finally { FreeLibrary(mod); }
    }
}
"@

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'bin\ZanpharsWinSuite.exe'
$shortcut = Join-Path (Split-Path -Parent $root) "Zanphar's WinSuite.lnk"

if (-not (Test-Path $exe)) { throw "not built: $exe" }

$count = [IconProbe]::ExtractIconEx($exe, -1, $null, $null, 0)
"icon resources in exe : $count"

if ($count -eq 0) { throw 'the exe carries no icon resources' }

$frames = [IconProbe]::GroupFrameCount($exe, 101)
"frames in that group   : $frames"

$large = New-Object IntPtr[] 1
$small = New-Object IntPtr[] 1
$got = [IconProbe]::ExtractIconEx($exe, 0, $large, $small, 1)
"extracted             : $got (large=$($large[0] -ne [IntPtr]::Zero) small=$($small[0] -ne [IntPtr]::Zero))"

# Compose what the shell would draw at 32 px and 16 px, on the desktop wallpaper grey.
$sheet = New-Object System.Drawing.Bitmap 300, 120
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::FromArgb(255, 246, 247, 249))
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$label = New-Object System.Drawing.Font 'Segoe UI', 10
$g.DrawString('shell icon 32px', $label, [System.Drawing.Brushes]::DimGray, 10, 8)
$g.DrawString('16px', $label, [System.Drawing.Brushes]::DimGray, 160, 8)
$big = [System.Drawing.Icon]::FromHandle($large[0]).ToBitmap()
$sm = [System.Drawing.Icon]::FromHandle($small[0]).ToBitmap()
$g.DrawImage($big, 10, 30, 96, 96)
$g.DrawImage($sm, 160, 70, 64, 64)
$label.Dispose()
$g.Dispose()
$shots = Join-Path $root 'shots'
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$outPng = Join-Path $shots 'icon-from-exe.png'
$sheet.Save($outPng, [System.Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
$big.Dispose()
$sm.Dispose()
[IconProbe]::DestroyIcon($large[0]) | Out-Null
[IconProbe]::DestroyIcon($small[0]) | Out-Null
"wrote                 : $outPng"

if (Test-Path $shortcut) {
    $wsh = New-Object -ComObject WScript.Shell
    $link = $wsh.CreateShortcut($shortcut)
    "shortcut target       : $($link.TargetPath)"
    "shortcut icon         : '$($link.IconLocation)'"
    if ($link.IconLocation -and -not $link.IconLocation.StartsWith($exe)) {
        "WARNING: shortcut icon is not the exe"
    }
    if ($link.TargetPath -ne $exe) { "WARNING: shortcut target is not the exe" }
} else {
    "shortcut              : not found at $shortcut"
}

# Explorer caches exe icons by path + timestamp; nudge it so the new artwork shows now.
[IconProbe]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED
Start-Sleep -Milliseconds 500

# Ask the shell itself what it will draw for the shortcut - the same code path Explorer,
# the Start menu and the taskbar use. If that is the generic app symbol we have not
# actually fixed the user-visible problem.
if (Test-Path $shortcut) {
    $info = New-Object IconProbe+SHFILEINFO
    $SHGFI_ICON = 0x100; $SHGFI_LARGEICON = 0x0
    $r = [IconProbe]::SHGetFileInfo($shortcut, 0, [ref]$info, [System.Runtime.InteropServices.Marshal]::SizeOf($info), ($SHGFI_ICON -bor $SHGFI_LARGEICON))
    "shell icon index      : $($info.iIcon)"
    if ($info.hIcon -ne [IntPtr]::Zero) {
        $bmp = [System.Drawing.Icon]::FromHandle($info.hIcon).ToBitmap()
        $path = Join-Path $shots 'shortcut-icon.png'
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        "shell draws           : {0}x{1} icon -> {2}" -f $bmp.Width, $bmp.Height, $path
        $bmp.Dispose()
        [IconProbe]::DestroyIcon($info.hIcon) | Out-Null
    } else {
        'WARNING: shell returned no icon for the shortcut'
    }
}
'ok'
