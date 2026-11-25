# HardWin

Hardens, tunes and stabilises a Windows 10 or 11 machine in place, and puts
every change back.

HardWin is a standalone tool in the Zanphar's WinSuite family. It reads fourteen
settings, shows which ones this machine already has, and changes only what you
ask it to - writing the original value down first, so **Revert** puts back what
was there rather than what the tool guesses Windows wanted.

```
HardWin.exe             open the window
HardWin.exe --audit     print what every check reads; changes nothing
HardWin.exe --selftest  prove the catalog, the journal and the service encoding
HardWin.exe --apply=id  tick that row (or those rows, comma separated) and harden
                        them; the elevation hand-off carries the selection this way
HardWin.exe --help      the same list on the console
```

## What it checks

### Security

| Row | Setting | What it buys |
|---|---|---|
| SMBv1 file sharing | `HKLM\SYSTEM\CurrentControlSet\Services\LanmanServer\Parameters\SMB1 = 0` | Closes the 2017-era sharing protocol WannaCry spread through. Windows 11 never ships it; a Windows 10 machine that has it turned on is one bad packet away from the same problem. |
| LLMNR name lookup | `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient\EnableMulticast = 0` | Stops the multicast name resolution that spoofing tools answer before DNS does, so a name on the local network resolves from DNS only. |
| AutoRun and AutoPlay | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoDriveTypeAutoRun = 0xFF` | A USB stick or a mapped drive no longer offers to launch what is on it. |
| SmartScreen for apps | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SmartScreenEnabled = "Warn"` | Warns before software Windows has never seen before is allowed to run. An unset value is Windows' own default and reads as hardened. |
| PowerShell script-block logging | `HKLM\SOFTWARE\Policies\Microsoft\Windows\PowerShell\ScriptBlockLogging\EnableScriptBlockLogging = 1` | Records every script block in the PowerShell operational log, so what a script did can be read back afterwards. |
| Defender blocks unwanted apps | `HKLM\SOFTWARE\Policies\Microsoft\Windows Defender\PUAProtection = 1` | Turns on the block level that stops most bundled adware and toolbars. |

### Performance

| Row | Setting | What it buys |
|---|---|---|
| Visual effects: best performance | `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects\VisualFXSetting = 2` | Drops the animation, shadow and fade work the desktop does for nothing. |
| Startup program delay | `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Serialize\StartupDelayInMSec = 0` | Explorer stops holding your sign-in programs back for ten seconds. |
| Menu show delay | `HKCU\Control Panel\Desktop\MenuShowDelay = "100"` | Menus open in 100 ms instead of 400 ms. |

### Reliability

| Row | Setting | What it buys |
|---|---|---|
| Core services restart themselves | failure actions on `EventLog`, `Winmgmt`, `WinDefend` | A service that fails restarts after a minute instead of staying down until the next boot; the counter resets a day later so a monthly hiccup is still restarted. |
| Automatic maintenance | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\Maintenance\MaintenanceDisabled = 0` | Keeps the idle upkeep Windows does - defrag, health checks, update staging - switched on. An unset value is the Windows default and reads as hardened. |
| No forced restart with programs open | `HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU\NoAutoRebootWithLoggedOnUsers = 1` | Windows waits for you to save your work instead of restarting under it. |
| Fast startup off | `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power\HiberbootEnabled = 0` | A hybrid shutdown carries the kernel session across a power cycle, which is where stale driver and file-system state come from; a full boot is slower and predictable. |
| Automatic crash dump | `HKLM\SYSTEM\CurrentControlSet\Control\CrashControl\CrashDumpEnabled = 7` | The automatic memory dump gives a second failure something to be diagnosed from, instead of a small dump that may not hold the stack. |

## The journal

Nothing is written until the original value has been saved:

- administrator checks journal under `HKLM\SOFTWARE\ZanpharsWinSuite\HardWin\journal`
- per-user checks journal under `HKCU\SOFTWARE\ZanpharsWinSuite\HardWin\journal`

Each entry records whether the value existed at all, its type, its data, and
whether the tool had to create the key. **Revert** restores exactly that: a
value that was not there is removed again, and a key this tool created is
deleted once its last value is gone. Failure actions for the three services are
saved as bytes and decoded again on the way back, so an existing recovery
policy is replaced rather than flattened.

The journal is kept in the hive that owns the value, so the same administrator
can revert a machine-wide change from any account. Reverting needs the same
rights applying did.

## Running it

- The per-user rows work as a standard user; the administrator rows show
  **needs administrator** and the window offers **Restart as administrator**
  (the manifest is `asInvoker`, so nothing prompts until you ask). Whatever is
  ticked travels with that prompt as `--apply=id,id`, so the elevated copy opens
  with the same rows ticked and hardens them itself instead of starting empty.
- **Every row is ticked on its own.** A click anywhere on a row ticks or
  unticks it - the box on the left and the row's border show the selection -
  and **Select all** / **Clear** at the end of the status line tick or untick
  the whole list. The status line counts both: "3 of 12 hardened - 2 selected".
- **Harden selected** and **Revert selected** act on the ticked rows only, in
  catalog order, one at a time: **Harden selected** skips the rows already in
  that state and reports how many were already done, and **Revert selected**
  skips rows with no journal entry (there is nothing this tool would be putting
  back) and says how many those were. Refusals are named row by row, so a
  selection is never reported as done when one row was left alone.
- **Clear** then re-tick is the way to act on a single row, and a tick survives
  **Re-check** - the selection belongs to the list, not to a run of the audit,
  so it is still there after a fresh read.
- Some rows want a restart to take effect everywhere: SMBv1, LLMNR and the
  Windows Update policy are read by components that start with Windows.
- **The window fits the screen, at any scaling.** It is 940 logical pixels wide
  and tall enough for the whole catalog - header, all fourteen rows and their
  group headings, and the footer - with one row of slack for the scroll snap.
  That height is worked out from the DPI of the monitor the window actually
  lands on, after it exists and before it is ever shown, so no frame at the
  96-DPI placeholder size is painted and the caption and borders are measured
  at the real DPI rather than the system one. The result is clamped to the work
  area - 150 % on a 768-pixel display leaves about 485 logical pixels, which a
  fixed-height window could not honour. The row list is a viewport in that
  space: it scrolls a whole row per wheel notch, ends on a row's bottom edge,
  draws only rows that are at least half in view, and shows a slim position bar
  on the right. Dragging the window onto a monitor with a different scale re-runs
  that same fit at the new DPI instead of scaling the old frame, so the window
  lands on a height that shows whole rows rather than half a card - which is why
  `--selftest` can prove it without a second monitor or any change to display
  settings: it builds the real window hidden, gives it two different heights,
  sends it `WM_DPICHANGED` in-process and checks that the height it settles on
  depends on the scale and not on where it started. Rows are counted by walking
  the list the way it is drawn, group
  headings included, so a short window shows as many whole rows as fit instead
  of losing one to rounding - and the position bar appears only when there is
  genuinely more list than window. The window is resizable (minimum 620 x 430
  logical, enforced in
  `WM_GETMINMAXINFO`), and every metric and font is scaled by the monitor's DPI,
  so the 150 % picture is the 100 % one at 1.5 x rather than a different layout.

## Icon

The program carries its own icon: the name **Hardened** over **Windows** on the
suite's dark card, with a padlock behind it. It is embedded as resource id 2
(`2 ICON "app.ico"` in `res\app.rc`) and set on the window class for both the
title bar and the taskbar button.

The lock is a watermark, not a second subject: its body and shackle are two dim
slate tones a single step above the card, so the words stay the mark. The keyhole
is drawn only at 48 px and up, where there are pixels to spend on it. Both words
are measured and scaled to the same width rather than set at a guessed point
size - "Hardened" is the longer one, and two lines that end at different edges
look tilted - so neither can overflow the rounded corners.

Below 32 px the words are dropped: eight and seven characters in a 16-pixel tile
is under two pixels a letter, which renders as a smudge, and 16 px is what the
title bar and small icon views use. Those two frames carry a simplified mark
instead — a tight **HW** on one line, drawn as glyph outlines at the largest size
the card takes and centred in the same near-white as the words. One row, not two
stacked: stacking split the tile into five pixels per letter, so the letters
crowded each other as much as the tile crowded them. Outlines rather than hinted
text, because at five pixels of cap height hinted strokes fuse; and no lock
behind them, because a dim silhouette competing for those same few pixels only
blurred the letters. The keyhole still appears at 48 px and up in the large
frames, where it has room.

```
powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
```

redraws the artwork and rewrites both files it is generated into:

- `res\app.ico` — 16, 24, 32, 48 and 64 pixels as uncompressed 32-bit DIBs (the
  form every shell renders) plus 128 and 256 as PNG, for Explorer's large views
  and the taskbar.
- `res\icon-preview.png` — the 256-pixel render, for anything outside the
  binary: a shortcut, an installer, a store listing or the website.

## Build

```
build_mingw.bat
```

or from the suite root, `build_all.bat`, which builds it into `bin\HardWin.exe`
with the rest of the suite. Either way the result is a static, self-contained
executable that imports only DLLs Windows itself ships.

```bat
HardWin.exe --selftest
```

runs the thirteen checks that need no administrator rights: the catalog's shape
and documented values, that an audit writes nothing, the journal round-trip on a
throwaway key, that an applied value is read back before it counts as done, that
a batch skips a row already in the wanted state without writing to it, that a
DPI change refits the window from the catalog, and the service-action encoding.
It prints one
`RESULT: n passed, 0 skipped, 0 failed.` line and exits non-zero if anything
failed.

## Dependability

- **A write counts only once it reads back.** After every write the value is
  re-read and compared with what was asked for. If the effective value did not
  take - a group policy, a filter driver, another tool - the original is put
  back immediately and the row reports why, instead of showing a tick that the
  machine does not honour. The service row confirms from the service manager
  that the recovery actions it set are the ones in force.
- **Everything is logged.** `%LOCALAPPDATA%\HardWin\hardwin.log` gets one line
  per applied, reverted, refused or unverified change, with a timestamp, the
  row's id and the reason, so an incident can be read back afterwards. It is
  under `LOCALAPPDATA` so the per-user rows log without an elevation prompt.
- **One copy runs at a time.** A named mutex keeps a second launch from opening
  a window that would audit and write the same journal concurrently; the second
  launch brings the running window to the front instead. A launch that carries
  rows for an elevated copy is the exception: it is the replacement for the
  window that is still on screen, so it waits a few seconds for the mutex
  rather than exiting and leaving the staged rows unapplied.
- **Drift is reported, not hidden.** On the first read of a session, any row
  that still has a saved original but no longer reads hardened is named in the
  footer - something else changed it back - with the way to restore it.

## Licence

BSD 3-Clause, Copyright (c) 2026-2027 Charles McDonald. See `LICENSE.md` in the
suite root.
