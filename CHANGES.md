# Changelog

All notable changes to **Zanphar's WinSuite** are documented here.

The suite is versioned as a bundle (SemVer); each program keeps its own
version in its `.rc` version block. Dates are the dates the change landed in
this folder.

## [2.1.0] - 2026-11-25

### Added

- **Browser Cleaner recognises Internet Explorer.** The catalog and the suite's
  Browsers page now include IE alongside Chrome, Edge, Brave, Vivaldi, Opera,
  Opera GX and Firefox, with its own selectable traces: **History** (the
  dedicated WinINet History folder, with the shared WebCache database
  deliberately left alone because it also holds cookies and other WinINet
  state), **Cookies** (INetCookies and the roaming Cookies folder), **Cache**
  (INetCache), and **Session** (the IE recovery folder). It is detected and
  closed like the other browsers; Passwords and Autofill are not claimed for IE
  because it does not store them in files this cleaner removes.

### Changed

- **System Turbo's dashboard can be resized and maximized.** The window now
  uses a thick frame and a `WM_GETMINMAXINFO` floor of 660 x 752 client pixels
  (DPI-scaled), so it cannot be dragged smaller than the layout it was designed
  for. Cards stretch with the client width, the action buttons share the full
  row, the memory card fills the remaining height, and painting is double-
  buffered at the live client size so resizing never exposes stale pixels or
  breaks the click targets.

## [2.0.0] - 2026-11-25

### Removed

- **Defrag and TRIM leave Internet Connection Utility.** Disk optimization belongs with the disks,
  not a connection doctor. The **Defragment hard disks** and **TRIM solid-state drives** repairs,
  their storage checks (media type, free-space fragmentation, TRIM support and delete
  notifications), and the `disks` module behind them are gone from ICU. The suite's **Storage**
  page now owns both actions, so there is one place to pick a volume and see the result.

### Added

- **Zanphar's WinSuite gains a Storage page.** The suite window now has a **Storage** tab for
the one storage job it was missing: each local volume is listed with the media type Windows
reports through the seek-penalty query, its free space, and the mode Windows' own
defragmenter will use — **Defragment** (`defrag.exe /D`) on a mechanical disk, **TRIM**
(`defrag.exe /L`) on a solid-state drive. A volume whose media type cannot be read is shown
with a disabled **Unavailable** button instead of being guessed at. Pressing Defragment or
TRIM opens a per-volume confirmation that names the drive, its label, the media and the exact
mode (`/D /H` for mechanical, `/L` for solid state), with No as the default, before the one
administrator prompt. While `defrag.exe` runs, Refresh and every volume button are disabled;
the page shows elapsed time, the percentage the defragmenter reports when available, and a
progress bar. **Cancel** terminates the child process, and closing the window cancels a job
rather than leaving it orphaned. The Windows defragmenter is resolved from System32 instead
of searching beside the elevated suite; unknown, optical and network media are refused before
a process starts. The volume list follows the mouse wheel and its scrollbar thumb can be
dragged; presses are keyed by drive letter and the row hit rects are rebuilt at the release
position, so a press after scrolling or dragging still lands on the row under the pointer. The
suite's read-only Storage lists on Dashboard and System are unchanged.

- **PCSpeedTest's CSV says which clock timed the graphics rows.** A new `gpu_timing`
  column records `timestamp` for the three GPU figures, which are measured with D3D11
  timestamp and disjoint queries, and `wall` for a build that has to use elapsed wall
  time instead; `mixed` covers a run with both, and the column is empty when no graphics
  figure was recorded. Rows written before the column existed migrate with it empty, so
  the wall-clock rows that could not previously be told apart now read as legacy rather
  than mixing in as timestamped runs. The header migration adds the column in place and
  keeps every run.

- **Internet Tuning Wizard puts the Nagle switch with the other switches.** The
  adapter-specific Nagle and delayed ACK card sat under the adapter list on the connection
  step, where it read as a note rather than a control. It is now a full-width switch below
  the four machine-wide switches on the Tuning step, names the connection chosen in step 1,
  and the review and result pages list it when it is on (and omit it when it is left off).

- **Internet Tuning Wizard's adapter scrollbar can be dragged.** The thumb follows the
  pointer from press to release instead of only paging when the track beside it is
  clicked, and the rows are laid out for the new offset before the next press is
  hit-tested, so a click straight after a drag selects the adapter under the cursor.

- **Home Policy ships a drift check for its catalog.** `HomePolicy\tools\check-admx.ps1`
  reads `src\catalog.cpp` and compares every row with the ADMX files in
  `C:\Windows\PolicyDefinitions` on key, value name, scope and enabled/disabled data,
  then prints one line per value and `CLEAN` or `DRIFT`, exiting 1 on drift so it can
  gate a change. Inverted data is reported as such rather than as drift, because the
  catalog is effect-oriented; Edge and Windows Script Host rows are listed with the
  reason they have no ADMX. Offline and read-only.

### Changed

- **Home Policy's catalog was audited against the ADMX files Windows ships.** All 37
  rows were matched by value name and key against `C:\Windows\PolicyDefinitions`: 36
  agree exactly with their policy definition, and the last (Windows Script Host) is a
  documented registry setting rather than an administrator-template value, which the
  README now says. Three rows described the wrong thing and were corrected: the
  activity-history row was titled as if it *kept* history while writing the two values
  that stop it being recorded, the Edge row claimed to remove promotional tabs when
  `ShowRecommendationsEnabled` only covers feature recommendations, and the no-auto-
  restart row now carries Microsoft's own condition (scheduled installs) and its
  documentation says the policy "doesn't work exactly as per description". The driver-
  exclusion row is tagged **Not documented for Home**; the two update-deferral rows keep
  their **Home ignores this** tag, which the ADMX editions confirm.

- **HardWin's elevation hand-off carries the ticked rows.** *Restart as administrator*
  used to launch a bare copy of the program, so the rows the user had ticked were left
  behind and the new window started empty - and because the window that launched it was
  still on screen, that copy could exit on the single-instance mutex before writing
  anything. The staged ids now travel on the command line as `--apply=id,id`, the
  receiving window ticks exactly those rows and hardens them itself, and a launch that
  carries them waits for the window it replaces instead of bailing out. Nothing ticked
  is still a plain restart.

- **HardWin's 16 and 24 px frames carry a tight "HW".** Below 32 px eight or
  seven characters come out under two pixels a letter, and the title bar uses
  exactly those frames. They now draw **HW** on one line as glyph outlines, sized
  to the card and placed on whole pixels, with the padlock and keyhole dropped
  there instead of competing for the same few pixels. Stacking the letters was
  tried and rejected: two rows of five pixels crowded each other. 32 px and up
  still draw the words, and the keyhole still appears from 48 px.

### Added

- **PCSpeedTest can empty the CSV from a scheduled task.** `--empty` is the
  console equivalent of the results reader's **Empty file**: it rewrites
  `results.csv` with the header alone, prints how many runs went and the path it
  emptied, reads the file back to prove the header survived, and exits 1 when
  the rewrite fails — so a task's last run result means something and no window
  ever appears.

- **PCSpeedTest gains a third graphics row, GPU - frames.** The compute and
  bandwidth rows say what the card can do, not what a game gets, so the same
  Direct3D 11 device now renders a simple scene offscreen — a cleared 1080p
  target, a depth buffer and 4096 blended quads spun by the vertex shader in one
  instanced draw — for 0.5 s of GPU time and reports frames per second. It is
  timed on the GPU's own clock like the other two rows, framed per frame so the
  slice is the work rather than the gaps between submissions, and the note says
  "GPU time only" so the figure is not read as a measured refresh rate. The
  catalogue is fifteen metrics, `gpu_fps` is the new `results.csv` column
  (appended last, so older files still load and older rows read as empty), and
  the window's row list grew from fourteen to fifteen.

### Fixed

- **Internet Connection Utility's TRIM repair is bounded and reports as it goes.** A heavily
  fragmented SSD could queue tens of thousands of 128-range requests in one run and show nothing
  until it finished, so a slow drive looked hung. Each run now sends at most 32,768 ranges (256
  requests), reports ranges sent and bytes trimmed to the Overview hero and the Repairs page while
  it works — the footer bar runs during repairs too — and records the next free range under
  `HKLM\SOFTWARE\Zanphar's WinSuite\InternetConnectionUtility`, so another run continues instead
  of repeating the prefix. The result line states how much was trimmed and whether the cap was
  reached; a complete run removes the resume point.

- **Internet Tuning Wizard no longer resets switches that were left off.** A run
  with the Nagle switch off still deleted `TcpAckFrequency`, `TCPNoDelay` and
  `TcpDelAckTicks` and reported `Nagle is back to the Windows default`, although the review
  page never listed that removal; the other four machine-wide switches were reset the same
  way. Apply now writes only the switches that are on, the review lists exactly what it will
  write or remove (the automatic DNS choice remains the one removal, clearing the adapter's
  override), and the result page has a row only for what actually ran. **Restore Windows
  defaults** still removes every value the wizard manages.

- **Home Policy's tags no longer squeeze the registry path.** A tagged card reserved
  150 logical pixels at the end of its path row for the caveat, which cut the Copilot
  row at `...\Windows\Windows` and hid its value name at both 100% and 150% scaling.
  Tagged cards are now one line taller, the caveat sits under the path in amber, and the
  path runs the full card width, so `...\WindowsCopilot  -  TurnOffWindowsCopilot` reads
  whole at both scales.

- **Internet Tuning Wizard no longer calls a restored DNS "automatic".** The first real
  GUI Apply/Restore run put the adapter's saved pair back (`127.0.0.1,1.1.1.1`) while the
  result page said `DNS is back to automatic on this connection`. The row now says the DNS
  is back to how it was before the wizard changed it, the restore summary no longer calls
  that a Windows default, the review note matches, and the README says the saved pair
  returns.

## [0.2.5] - 2026-11-24

### Added

- **Internet Connection Utility 1.4.0.0 — defrag and TRIM inside the repair list.**
  Two storage repairs join the catalogue: **Defragment hard disks** runs Windows' own
  `defrag.exe /D` on every mechanical volume, and **TRIM solid-state drives** sends TRIM for the
  free ranges of every SSD and re-enables `DisableDeleteNotify` when Windows had it switched off.
  A new `disks` module reads the media type from `StorageDeviceSeekPenaltyProperty` and the bus
  from `StorageDeviceProperty`, so the doctor offers the repair that suits the drive instead of
  the user guessing. Two checks feed them: free-space fragmentation measured from the NTFS volume
  bitmap, and TRIM support plus delete notifications. The TRIM request is
  `IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES` over clusters the file system has already released,
  so no allocated block is ever hinted as discardable. Both repairs carry `ADMIN`.
- **Internet Connection Utility** (`bin\InternetConnectionUtility.exe`) — a ninth
  standalone program, now part of the suite: a connection doctor that measures
  why a connection is failing, dropping or slow and repairs what it can. About
  thirty checks cover the Winsock/TCP stack, adapter and link state, DHCP and
  addressing, routing and interface errors, per-server DNS probes, ICMP
  latency/jitter/loss, the TCP :443 handshake and HTTPS egress, captive
  portals, a short throughput sample, retransmissions, path MTU, Windows
  tuning (auto-tuning, NIC power, firewall profiles, core services, proxy/PAC)
  and Wi-Fi (signal, band, channel occupancy, security, power saving). Results
  stream in as each probe finishes and become a 0–100 score; the Wi-Fi tab
  shows nearby networks and 2.4 GHz occupancy.
- **Twenty-one repairs, labelled by risk.** Each repair carries `ADMIN`,
  `CHANGES` or `RESTART`, risky ones ask first, and a repair that needs
  elevation offers to relaunch the same executable as administrator. The
  Activity tab logs everything measured and changed, and **Copy report** puts
  a plain-text summary on the clipboard. A hosts reset leaves a timestamped
  `.bak` beside the original.
- **Internet Tuning Wizard** (`bin\InternetTuningWizard.exe`) — an eighth
  standalone program: a three-step wizard that writes six documented network
  settings on Windows 10 and 11 — network throttling off for the whole PC
  (`NetworkThrottlingIndex = 0xFFFFFFFF`), IPv6 off (`DisabledComponents`
  `= 0xFF`), Nagle and delayed ACK off on one adapter
  (`TcpAckFrequency`/`TCPNoDelay`), and a DNS resolver for that adapter
  (`NameServer`: system default, Cloudflare, Google or Quad9). The connection
  comes from the adapters Windows reports, the review page lists every change
  before anything is written, and **Restore Windows defaults** removes every
  value again.
- **Its Apply asks once, and reports per value.** The wizard opens `asInvoker`
  so the whole review can be read without a prompt; Apply hands the plan to an
  elevated copy on the command line, which writes the values, reads them back
  and answers through a small text file, so the window shows a green or red row
  for each one. DNS is flushed after a write. `--list` prints every adapter,
  its resolvers and what the six settings currently read; `--selftest` proves
  the provider table, the plan encoding used for the handoff, the review text,
  the registry engine on a scratch key and the adapter enumeration.
- **Every connected adapter, in a list that scrolls.** Step 1 no longer stops
  at the first five adapters and no longer skips VPN or tunnel interfaces;
  every connected adapter is offered, and the list scrolls with the wheel,
  arrow keys or its scrollbar when it outgrows the page, clipped to its own
  viewport so rows never paint over the DNS block.
- **DNS that survives the lease.** The resolver write now covers both registry
  sides: `NameServer` (what Windows prefers) and `DhcpNameServer` (what a
  reconnect can fall back to), after saving the adapter's previous pair so
  **Restore Windows defaults** can put the DHCP-provided list back instead of
  leaving the adapter without one. Measured on Windows 11 26300: after
  `ipconfig /renew`, `NameServer` still held `1.1.1.1,1.0.0.1` while Windows
  rewrote `DhcpNameServer` to the router's `192.168.4.1`; restore returned both
  to their pre-apply values. A seventh self-test covers the save/write/restore
  round trip on a scratch key.
- **Two more machine-wide switches, and Nagle gets a face.** Step 2 now also
  offers the multimedia CPU reserve off (`SystemResponsiveness = 0`, the
  companion of network throttling) and TIME_WAIT capped at 30 seconds
  (`TcpTimedWaitDelay = 30`, for clients that exhaust ports), in a 2x2 grid that
  fits the smallest window. Step 1 finally exposes the per-connection Nagle
  switch the review page had been describing, now writing `TcpDelAckTicks = 0`
  alongside `TcpAckFrequency`/`TCPNoDelay`; `--list` and `--help` report all six
  settings, and the self-test covers the new constants and the six-line review.
- **A result page you can act on.** The page that reports the elevated copy's
  rows now keeps **Back** to the review and **Restore Windows defaults** (armed
  on the first click), so a run can be undone without walking the wizard again;
  declining the UAC prompt lands there with a red row instead of stranding the
  window.
- **A speedometer icon**, drawn by `tools\make_icon.ps1`: the accent arc, a
  bright needle and an accent hub on the suite card, with a thicker arc and no
  tick dots at 16 and 24 px.
- **Home Policy** (`bin\HomePolicy.exe`) — a seventh standalone program: a
  Group Policy Editor for Windows 10 Home and Windows 11 Home, where `gpedit.msc`
  does not exist but the components that read policies still honour the same
  `HKLM\SOFTWARE\Policies` values. Thirty-seven settings in seven categories
  (Windows Update, privacy and telemetry, Start/search/taskbar, File Explorer,
  system and apps, security, Edge), each row one named policy value with a plain
  sentence and the three states the official editor offers: **Not configured**,
  **Enabled**, **Disabled**. Not configured removes the value rather than writing
  a guess, and Disabled writes the policy's off-data so the three states stay
  distinguishable in the registry instead of collapsing into "absent".
- **The copyright sits in the footer.** A hairline rule separates the action
  row from a bottom row that carries `© 2026-2027 Charles McDonald` on the left
  and the administrator link on the right - faint enough to stay out of the way,
  present on every screen size.
- **Home Policy stages every choice before it writes one.** A click marks the
  card, counts in the footer and enables **Discard**; the first **Apply changes**
  asks for administrator rights once and reopens the window elevated rather than
  letting each row fail with Access denied. The manifest stays `asInvoker`, so the
  catalog is fully readable - with `--list` too - before any prompt, and declining
  UAC leaves nothing changed. **Back up...** writes a UTF-16 `.reg` containing
  every catalog value that is currently configured.
- **Every row audited against the ADMX files on this build.** Each path, value
  name and on/off pair was checked against `C:\Windows\PolicyDefinitions` and
  Microsoft's policy documentation, and several were wrong: window shaking, Open
  with and Copilot pointed at keys Windows no longer reads, thumbnail caching and
  the Windows-key shortcuts were written machine-wide though their policies are
  user-scope, Windows Script Host aimed at a `Policies` key that does not exist
  instead of `SOFTWARE\Microsoft\Windows Script Host\Settings`, and Edge's
  deprecated `PromotionalTabsEnabled` became `ShowRecommendationsEnabled`.
  OneDrive's row is renamed for what `DisableFileSyncNGSC` actually does - it
  stops the whole sync client, not just personal accounts. The telemetry row is
  gone, because Microsoft documents `AllowTelemetry = 0` as equal to `1` on Home
  and the row could not change anything; the two update-deferral rows now read
  **Home ignores this**, since deferral is a Windows Update for Business feature
  documented for Pro, Enterprise and Education only; Cortana and Copilot carry
  **Deprecated by Microsoft** instead of the old umbrella tag.
  `--selftest` proves the catalog (no empty rows, no duplicate
  targets, at least thirty settings), the three-state mapping, and an
  enabled/disabled/not-configured round trip on a scratch key under
  `HKCU\Software\ZanpharsWinSuite\HomePolicy\selftest` that it deletes again.
- **Its icon is the settings mark in two tiers.** Three slider tracks with **GP**
  over them: the sliders are a watermark from 32 px up, and at 16 and 24 px - the
  frames the title bar picks, where two letters are a smudge - they are drawn
  alone, full width and brighter, in the same `res\app.ico` that Explorer reads.

- **HardWin** (`bin\HardWin.exe`) — a fifth standalone program that hardens,
  tunes and stabilises a Windows 10 or 11 machine in place. Fourteen settings
  in three groups: six security rows (SMBv1 off, LLMNR off, AutoRun/AutoPlay
  off, SmartScreen on, PowerShell script-block logging on, Defender
  unwanted-app blocking on), three performance rows (best-performance visual
  effects, no ten-second startup-program delay, a 100 ms menu delay) and five
  reliability rows (restart-after-failure actions on Event Log, WMI and
  Defender, automatic maintenance left on, no forced restart while programs are
  open, fast startup off so every boot is a full one, and the automatic crash
  dump left on so a second failure can be diagnosed).
- **HardWin only counts a change once it reads back.** After every write the
  value is re-read, and a row whose effective value did not take - a group
  policy, a filter driver, another tool - is rolled back on the spot and named
  in the footer rather than shown as hardened; the service row re-queries the
  service manager to confirm the recovery actions in force. Every applied,
  reverted, refused or unverified change is appended to
  `%LOCALAPPDATA%\HardWin\hardwin.log` with a timestamp and the reason, a named
  mutex keeps a second copy from writing the same journal concurrently, and the
  first read of a session reports any row that still has a saved original but no
  longer reads hardened, so drift is a named fact instead of a silent one.
  Harden selected and Revert selected both go through one engine batch routine
  that owns the skip rules (already hardened, no saved original) and the refusal
  list, so the window has a single loop to keep in step rather than its own
  copy of the engine's.
- **HardWin's window is measured at the monitor's own DPI.** The height the
  catalog needs is computed from the real DPI once the window exists and applied
  before the first paint, instead of being passed to `CreateWindowEx` while the
  process still assumes 96 DPI, and the caption and borders are measured with
  `AdjustWindowRectExForDpi` so the client area is not short by a row at 125 % or
  150 %. The window is resizable down to 620 x 430 logical pixels, and below the
  full catalog height the row list scrolls rather than running under the footer.
  Moving the window onto a monitor with a different scale recomputes that same
  catalog height at the new DPI and then clamps it to the new monitor's work
  area, instead of multiplying the old frame by the DPI ratio - scaling keeps
  whatever height the window happened to have, so a window dragged from a
  100 % monitor lands on half a row at 150 % and a window dragged the other way
  wastes the top of the list. The self-test drives the handler in-process
  (`WM_DPICHANGED` with a null suggested rectangle) and fails if the height it
  settles on depends on the height it started from.
  Rows in that viewport are counted by walking the list the way it is drawn,
  group headings included, so a short window shows as many whole rows as fit.
  The position bar appears only when the list genuinely overflows.
  Every write saves the original value first - its type, its bytes and whether
  it existed at all - under the hive that owns the setting, so **Revert** puts
  back exactly what was there, removes a value this tool created and drops a
  key it had to create. A setting Windows already ships hardened reads as
  hardened when it is unset instead of being "fixed"; a row the process cannot
  reach reads **needs administrator** rather than open. Every row carries a tick
  of its own - a click anywhere on the row ticks or unticks it - with **Select
  all** and **Clear** at the end of the status line, and the line counts both
  readings ("3 of 12 hardened - 2 selected"). **Harden selected** and **Revert
  selected** act on the ticked rows only, in catalog order: hardening skips and
  counts the rows already in that state, reverting skips the rows with no saved
  value, and any refusal is named so a selection is never reported as done when
  a row was left alone. A tick survives **Re-check**, because the selection
  belongs to the list rather than to a run of the audit. The manifest is
  `asInvoker`, so the window offers an administrator restart for the rows that
  need it; `--audit` prints what every check reads without
  writing anything, and `--selftest` runs twelve checks over the catalog, the
  journal round-trip, the read-back after a write, the batch skip rule, the
  window's response to a DPI change and the service-action encoding, ending in
  the usual `RESULT:` line. The window opens at 940 x 700 logical pixels but is clamped to
  the work area of the monitor it sits on, and the row list became a viewport in
  that space: it scrolls one whole row per wheel notch, ends on a row's bottom
  edge, draws a row only once half of it is in view and shows a slim position
  bar when there is more list than window - which is what makes fourteen rows fit
  a 768-pixel display at 150 %, where the work area is about 485 logical pixels.
  Every metric and font is DPI-scaled, so the 150 % window is the 100 % layout
  at 1.5 x rather than a second layout to keep in step. The program also carries
  its own icon - a padlock behind the initials **H** (top left) and **W** (lower
  right) on the suite's dark card - embedded as resource id 2 and set on the
  window class, so the title bar and the taskbar button carry it; the artwork is
  regenerated by `HardWin\tools\make_icon.ps1`, which writes `res\app.ico`
  (16/24/32/48/64 as DIBs, 128/256 as PNG) and a 256-pixel
  `res\icon-preview.png` for use outside the binary. Folders: `HardWin\src\`
  (`harden`, `main`), `HardWin\res\` and `HardWin\tools\`; `build_all.bat`
  gained its recipe row.

- **PCSpeedTest** (`bin\PCSpeedTest.exe`) — a sixth standalone program that
  measures processor, memory, disk, LAN and internet speed and appends every
  run to a CSV for comparison. Twelve rows in four groups: two processor rows
  (a dependent integer and float loop on one thread and on every thread),
  two memory rows (64 MiB copy bandwidth, and the latency of a dependent load
  over a shuffled 32 MiB chain), two disk rows (192 MiB written and flushed,
  then read back with `FILE_FLAG_NO_BUFFERING`), and six network rows: the
  default route's negotiated link speed, TCP loopback throughput, gateway
  latency, internet latency with jitter and loss, and HTTPS download and
  upload rates against `speed.cloudflare.com` with byte and time caps.
  A row that cannot be measured - ICMP filtered, no gateway, a server error,
  no disk room - carries the reason in the window and leaves its CSV column
  empty instead of writing a zero, and never stops the rest of the run.
  Every completed run appends one RFC 4180 line to
  `%LOCALAPPDATA%\PCSpeedTest\results.csv` with the timestamp, host,
  processor, core count, RAM and OS beside the figures; a single row measured
  from the window is not written, so the file holds only full runs. **View
  results** reads the file back as a window of its own - one card per run,
  newest first, with the timestamp, the machine and the twelve figures as four
  columns by three rows, a dash where a column was left empty - with **Open in
  spreadsheet** to hand the file to the default application, and a click on a
  card doing the same.
  `--run` prints the twelve figures and appends a line without a window;
  `--selftest` runs eleven engine checks with no network traffic and no
  leftover files, including the CSV reader over a synthetic file of the
  self-test's own making - a quoted host with a comma in it, a quoted cpu with
  a quote in it and one blank metric - so quoted fields, the blank column and
  the column count are proved on every build, and a second file under the older
  header proves the migration keeps its row and its figures; the user's
  results.csv is never opened.
- **PCSpeedTest names the machine it is on, not what the registry claims.**
  Windows 11 keeps `ProductName` = `Windows 10 Pro` for compatibility, so the
  version word now comes from the build number (22000 and above is Windows 11,
  `Windows Server ...` is left alone), with edition, display version and UBR
  beside it. Physical cores come from `RelationProcessorCore` instead of the
  thread count - the old line called an i7-6700K "8 cores" - and the
  architecture from `GetNativeSystemInfo`. The window draws it in two lines
  (hardware, then platform with the adapter and its speed) and `--run` prints
  the same. In the CSV, `cores` is now physical with a new `threads` column and
  `arch` beside it; a file under the older header is rewritten in place on the
  next append, moving its logical count to `threads` and correcting a Windows 10
  claim that sits beside a 22000+ build.
- **One copyright across all eight binaries and the pages that show it.** Every
  version resource now reads `Copyright © 2026-2027 Charles McDonald. BSD
  3-Clause License.` (Browser Cleaner keeps its "not affiliated with IObit"
  clause), the suite's Settings footer and licence dialog carry the same years,
  and `LICENSE.md` and the website's licence page match them. The two page
  strings and the self-test that looks for the licence line moved together, so
  the check still proves the dialog text is the licence the file holds. The
  Windows version each program reports was checked at the same time: the suite
  already names 11 by build number (22000 and up) and no other program prints a
  version word at all, so PCSpeedTest was the only place the registry's
  `ProductName` had been repeated.
- **PCSpeedTest records the graphics adapter and the memory modules too.** The
  description under the title is three lines now: the processor, cores and
  installed memory with the modules' speed and maker; the adapter attached to
  the desktop with its driver version; then the platform. The adapter comes from
  `EnumDisplayDevices`, its driver from the display class key whose `DriverDesc`
  matches it, and the memory from SMBIOS type 17 - the configured speed and the
  manufacturer, `mixed` when the modules differ and a range when their rates do.
  Every one of those fields is left empty when Windows will not report it rather
  than guessed, and the CSV grew `ram_speed`, `ram_maker`, `gpu` and
  `gpu_driver` columns; a file written under the older header picks the new
  columns up empty on its next append. A self-test check parses a firmware table
  the test builds itself, so the offsets are proved without hardware.
- **Every product starts in the centre of the work area.** Zanphar's WinSuite,
  DNS Protect, System Turbo and HardWin already placed themselves there;
  Browser Cleaner, PCSpeedTest (its window and the results viewer) and the
  Smart RAM card now do the same instead of taking whatever corner Windows
  picked. Each centres on the monitor the window lands on rather than a
  hard-coded screen, so a second display is handled the same way, and the card
  still remembers where it was dragged to - the centre is only where a machine
  that has never seen it puts it.
- **PCSpeedTest measures the graphics card too.** Two rows in a new Graphics
  group: a Direct3D 11 compute shader's multiply-add rate in GFLOP/s (four
  independent chains over 262144 lanes) and a 64 MiB texture copy's bandwidth in
  GB/s. Both are timed on the GPU's own clock with timestamp queries over a
  batch whose size is calibrated by one timed dispatch, so the gaps between
  submissions are not counted as work, and the queue is drained before the clock
  is read. A session with no Direct3D 11 device says so where the figure would
  be, like any other blocked row. The CSV grew `gpu_gflops` and `gpu_bandwidth`,
  the window and its results cards went to fourteen rows (four columns by four),
  and `--selftest` runs a dispatch as its fourteenth check: a plausible rate, or
  the reason there is none.
- **The results viewer can empty results.csv.** **Empty file** sits beside
  **Results folder** and **Close**; it asks first, naming how many runs go and
  defaulting to **No**, and then leaves the file with its header and nothing
  else - so the reader shows "holds a header and no runs" instead of cards, and
  the next run appends to a file that still knows its columns. The window
  reloads in place; nothing else in the folder is touched. A fourteenth
  self-test check appends two runs to a temporary file, empties it, and asserts
  the header survived and the reader found nothing.
- **HardWin's icon carries the name, not the initials.** The tile reads
  **Hardened** over **Windows**, with a padlock behind it as a watermark - body
  and shackle in two dim slate tones a step above the card, keyhole drawn only
  from 48 px up - so the words are the mark and the lock is what they stand for.
  Each line is measured and scaled to the same width, because "Hardened" is the
  longer word and two lines ending at different edges look tilted. At 16 and 24
  px the words are dropped instead of smeared - those frames, which are what the
  title bar and small icon views pick, carry the lock alone, larger, filling the
  tile, in brighter tones with the keyhole from 24 px up. So the mark degrades on
  purpose: lock, then lock under the name. The same `res\app.ico` is what the
  window class sets on the title bar
  and taskbar (resource id 2) and what Explorer reads from the exe, so the tile
  that ships is the tile that is drawn. The window follows the house layout - DPI-scaled, scrolls whole rows,
  a row click measures just that row, **Run all** becomes **Stop** while a run
  is in flight - and carries its own speedometer icon from
  `PCSpeedTest\tools\make_icon.ps1`. `build_all.bat` gained its recipe row.

The tree keeps its sources, its documents and the reference screenshots, and
nothing that a build can produce; every binary and the release package were then
built again from that clean tree.

- **Home Policy has a console mode for one setting.**
  `HomePolicy.exe --apply "Widgets board" disabled` resolves a title (any
  unique, case-insensitive part of one; an exact title wins), writes that row,
  reads the registry back and prints before / asked / reads plus the raw DWORD,
  exiting 0 only when the read-back matches, 1 when the write or read failed and
  2 for a bad or ambiguous argument. `not-configured` removes the value again,
  so one real policy can be applied and reverted from an elevated shell without
  opening the window. The internal hand-off flag is now `--staged`, so `--apply`
  means exactly one thing, and `--selftest` gained a sixth check that a partial
  title resolves to one row and that the three state words are understood.

### Changed

- **`.gitignore` covers build intermediates.** `*.o`, `*.obj` and `*.res` were
  invisible to the ignore file in some folders and tracked in others, which is how
  `BrowserCleaner/src/resources.o` came to be committed in the first place. The
  root file now ignores all three, together with `SmartRAM/build/`,
  `SmartRAM/build-mingw/`, `SmartRAM/dist/` and
  `BrowserCleaner/BrowserCleaner.exe`, so a local build leaves nothing behind for
  `git add` to reach. The README's repository tree follows the removals below and
  now says that the tree keeps no build intermediates.
- **The binaries and the package come from a clean rebuild.** `release.bat
  /build` wiped `bin\` and rebuilt all seven executables from the cleaned tree -
  62 self-test checks pass and every import table names a Windows DLL - then
  restaged `release\Zanphar-WinSuite-v0.2.4\` and the archive beside it, so the
  executables, the package and the copy the website hands out are one build.
  `website/downloads/` and the checksum printed on the download page were
  refreshed to the new archive.

### Removed

- **Per-project build trees.** `SmartRAM/build-mingw/` - twenty-eight files of a
  CMake/Ninja tree whose cached compiler paths named an older checkout - and the
  ignored `SmartRAM/build/`, `DNSProtect/mingw-override/` and
  `SystemTurbo/mingw-override/` folders. `SmartRAM/tools/build-mingw.ps1` and
  `package.ps1` still produce those trees on demand, so no maintainer step
  depended on them being in the repository.
- **Object files and other build output.** `BrowserCleaner/src/resources.o`, the
  suite's `build\app.res.o` and `build\agent.res.o`, the ignored
  `SystemTurbo/SystemTurbo.exe` sitting beside its sources, and the `build\`
  scratch area with its `warn\` folder and the deploy staging. `build.bat` and
  `build_all.bat` recreate whatever they need on every run.
- **Orphaned single-program executables and archives.**
  `BrowserCleaner/BrowserCleaner.exe` (3.4 MB) sat next to the sources it was
  built from, and `SmartRAM/dist/` held a per-program ZIP and checksum written
  before the suite packaged all six programs as one archive. No script, CI step or
  document referenced either copy: the only executables the project ships are
  `bin\*.exe`, and the only archive is the one `release.bat` writes.
- **Reference captures nothing was using.** The `shots\*.bmp` scratch renders; the
  `shots\*.png` and `SmartRAM\tools\shot_*.png` captures the README points at stay
  where they were.

### Fixed

- **Home Policy carries staged choices through elevation.** The unelevated
  window used to launch an elevated copy and drop its staging, and that copy
  could exit on the single-instance mutex while the first window was still
  alive - so nothing was written. Apply now encodes the staged rows as
  `--apply "index:state;..."`, hands them to the elevated window, closes
  itself so the lock is free, and the new window waits for the handover (up to
  ten seconds) instead of exiting. It writes exactly those rows, re-reads the
  registry, and reports what landed; a declined UAC prompt leaves the original
  window open with every choice still staged. `--selftest` gained a fifth
  check that encodes, parses and applies a staged list to a scratch key, and a
  live two-process run proved the waiting child consumes the list.

## [0.2.4] - 2026-10-24

Game Booster learns the long list of background services a gaming session does
not need, and the notification area becomes a place to start the suite and run a
memory clean from.

### Added

- **Game Booster lists the background services worth parking.** Twenty-eight rows
  cover connected user experiences and telemetry, the touch keyboard and
  handwriting panel, Netlogon, Xbox Live, Windows Image Acquisition, geolocation,
  the program compatibility assistant, WAP push routing, retail demo, Windows
  Error Reporting, Print Spooler, certificate propagation, enterprise app
  management, the biometric service, the Insider service, the network connection
  broker, Remote Desktop, the portable device enumerator, Parental Control,
  Windows Connect Now, smart cards, Windows Event Log, Remote Registry, the phone
  service, downloaded maps, AllJoyn, Fax and Secondary Logon. Each row shows the
  real service names and their live start type and state, so a row Windows
  already had off reads **Already off** and one whose services are not installed
  says so instead of offering a switch that would do nothing.
- **Turn off stops the service and sets it to Manual**, not Disabled, so Windows
  can still start one when something genuinely asks for it. The start type,
  whether the service was running and its delayed-autostart flag are saved first,
  and **Revert** puts each one back exactly; **Revert all** now restores the
  services along with the registry tweaks. Rows Windows leans on - Event Log,
  Remote Desktop, Netlogon - print their state in the warning colour, and none of
  the rows is part of the one-click profile: what is safe to park depends on the
  machine, so each one is a press of its own.
- **Start in the notification area.** A new Settings option, **Show on system tray
  at startup**, opens the suite straight into the tray with no window on the
  desktop; the icon's menu is the way in. If the shell refuses the icon the window
  is shown instead, so the suite can never run with nothing to click.
- **Regular and Deep Memory Clean in the tray menu.** The icon's menu now reads
  Open, Regular Memory Clean, Deep Memory Clean, Exit. Both items run exactly what
  the Memory page's buttons run, including the Settings prompts and the deep
  clean's administrator restart, so a clean no longer needs the window opened.
  The result arrives as a notification-area balloon, because the window it would
  otherwise be reported in is hidden.
- **Toast notifications can be switched off.** A Settings option gates the tray
  balloon for a clean started from the menu; with it off the clean still runs and
  the Memory page still carries the figures for the next time the window is open.
  It is on by default, and a settings file without the key reads as on, so
  upgrading never goes quiet on a machine that already had the reports.

### Changed

- The Settings page's WINDOW section is now four rows - close to tray, minimise to
  tray, start in the tray, toast notifications - and the page scrolls a little
  further on a 1024 x 768 window to reach the licence card.
- The Task Manager pages, their confirmation dialogs and the documentation use the
  name **Task Manager** throughout, and a second launch closes the copy that is
  already running instead of opening a second window.

### Fixed

- **Saved task rules apply without signing out.** Saving a rule now starts the
  helper immediately, so a program launched later in the same session picks up its
  affinity and priority; before, the rule only reached the next sign-in.
- **The affinity row shows every core.** The CPU chips wrap instead of running off
  the card, so a 64-thread machine can still reach its last cores.
- **A ticked task stays with its own process.** Windows reuses process ids, and a
  tick was held by the number alone, so a bulk **End task** could reach whatever
  program inherited the id of a process that had already ended. A tick now carries
  the process creation time as well, and both the tick and the highlight are dropped
  the moment that instance exits. Two new self-test checks pin the rule down.
- **The start-type chips change the whole selection.** **Automatic**, **Automatic
  (delayed)**, **Manual** and **Disabled** acted only on the highlighted row, so a
  list with several rows ticked changed one of them. They now set every ticked
  service in one press, skip the rows already in that state, ask for the
  administrator restart once, and report how many changed and which refused.
- **A hidden suite stops measuring and enumerating.** Parked in the notification
  area, the window is hidden rather than minimised, so the page polls kept running
  and the suite went on measuring the Cleanup folders, enumerating processes and
  reading services and volumes all day. Those refreshes now run only while a page is
  actually on screen; the automatic memory clean is untouched.
- **A service tick survives a change of capitals.** Ticks were matched by exact
  name, so a load that reported the same service with different case dropped them.
  The match is now case-insensitive, as the service database itself is.
- **The Game Booster list ends on its last row.** The scroll extent assumed the
  ADVANCED group was always drawn, so a machine whose tweaks all sit inside the
  profile could scroll past the background services into empty space.

## [0.2.3] - 2026-10-24

The Recycle Bin joins Cleanup, measured across every drive and emptied only after
a warning that names what is about to be destroyed.

### Added

- **The Recycle Bin is a Cleanup row of its own.** Cleanup now lists what has already
  been deleted, measured across every drive with one shell query, with the size and
  item count on the row and the word **permanent** beside the name. It arrives
  unticked, is left alone by **Select all** and is never written to the saved ticks,
  and **Clean selected** stops at a confirmation that names the size and count and
  says the items are deleted for good - because this is the one row where nothing
  goes to the Recycle Bin for safe keeping. Emptying reports only what actually
  left: the figures are read again afterwards, so an item another program is holding
  open is counted as left behind rather than as freed.

## [0.2.2] - 2026-10-24

Repackaged with the latency check finishing itself, a CI guard on every
binary's version resource, and a website serving the archive it describes.

### Added

- **Applying the profile takes the after-reading too.** **Apply profile** on
  Internet & networking now finishes the latency check by itself: once the
  values have landed and a baseline is in hand, it measures the host again on
  the worker and the card settles on the compared pair - "11 ms to 14 ms - 3 ms
  higher on average" - without a second press. A press with no reading yet still
  takes the baseline first and then applies, so one press now produces the whole
  before-and-after. An apply that changed nothing does not ping, a measurement
  already in flight is left to land as the after-reading, and a failed
  measurement leaves its error on the card without undoing the apply.
- **CI refuses an executable that names the wrong company.** A new step reads
  the Windows version resource of every `.exe` in the staged package - the
  strings Explorer shows under Properties → Details - and fails unless
  `CompanyName` is exactly `Charles McDonald` and `LegalCopyright` names him
  too. A missing resource, an empty company, the old `Zanphar` holder or a
  copyright line naming someone else each fail with a message naming the file
  and the field, so an archive whose binaries disagree with its `LICENSE.md`
  cannot be published.
- **CI refuses a package carrying the wrong licence.** A new step runs right
  after the document-parity check and reads the staged `LICENSE.md`, so it
  judges the file that would actually be published. The build fails unless the
  first line names the BSD 3-Clause License, the
  `Copyright (c) 2026, Charles McDonald` line is present, all three clauses and
  the warranty disclaimer are still there word for word, and the text contains
  no MIT permission grant - the tree is BSD 3-Clause only. A missing
  `LICENSE.md`, a different copyright holder, a renamed licence or a stray MIT
  paragraph each fail with a message naming the problem, so the publish job can
  never attach an archive under terms the repository does not use.
- **One press puts all four networking tweaks back to the Windows defaults.**
  **Restore Windows defaults** on Internet & networking sits beside Re-check and
  switches Nagle, throttling, the Games task profile and the QoS reservation off
  together, instead of a trip to each row. Each value is journalled before
  it is cleared, exactly like a single row's **Turn off**, so **Revert all**
  still puts back what another tool had set before the suite touched it. The
  button is only armed while at least one tweak is on, a partly applied profile
  is reset whole, and a press with nothing on answers "All four tweaks were
  already at the Windows default" rather than doing nothing quietly. The answer
  on the card names how many values were reset and what Revert still offers.

### Changed

- **The site is published, and it hands out the current archive.** The website
  now has a live address
  (<https://site-5b3563511c074235bb40fd74c965651c.freebuff.page/>), served as
  plain static files by a pass-through Worker. The copy of the archive in
  `website/downloads/` had been left behind by an earlier build, so the download
  page offered an old package while quoting its own checksum; it now mirrors
  `release/`, and the SHA-256 printed on the download page and in its Copy
  button is the current one
  (`1786ac933696467df5ace1c2001ac17d851da0367c584ad9b686db163f07988b`).
  The privacy page no longer says the site carries no analytics at all: it
  carries none of its own, but the host adds Cloudflare's cookie-free visit
  counter, and the page now says so instead of inviting a contradiction in a
  devtools network panel.
- **The latency comparison reads as one span.** With both readings in, the card
  states the change as "11 ms to 14 ms - 3 ms higher on average" instead of only
  the difference, so the two averages no longer have to be subtracted by eye.
  The line keeps the packet-loss note when it moved and its colour: green when
  the average fell, amber when it rose, muted when it did not move. A missing
  reading produces no line at all, and the Before/After figures with their
  min-max ranges stay above it.

### Fixed

- **The Services and Task manager lists only ever show whole rows.** Both lists
  used to clip at the footer, so the bottom row could be left half drawn; now
  the clipped area ends on a row's bottom edge and a wheel notch rounds to the
  nearest row, in both directions. The viewport keeps only as many rows as fit
  whole - up to one row shorter than before - so the list also starts on a row
  boundary after scrolling, and the count, ticks and footer are untouched. The
  same helper drives both lists, and the self-test checks the row boundary, the
  aligned scroll and the furthest offset.
- **Game Mode reads and switches correctly.** Windows has Game Mode on unless a
  value says otherwise, so the row now reads ON on a PC where nothing has ever
  set it, instead of showing OFF and offering a **Turn on** that changed
  nothing. **Turn off** writes the explicit off value (0) for both Game Mode
  values rather than deleting them, which would have left the feature running
  while the page reported it off; **Revert** still puts back what was there,
  including "nothing at all". The profile no longer rewrites a Game Mode value
  that is already on.
- **A refused Restore no longer strands a parked service.** The record of a
  service's original start type is kept until Windows accepts it, so a restore
  that lacks the rights, or that Windows refuses, can be retried instead of
  leaving the service at Manual with nothing left to say what it used to be.
  The record also carries the delayed-autostart flag, so Windows Search and
  SysMain come back as Automatic (Delayed) rather than plain Automatic.
- **The profile says when it could not do something.** Each refused plan
  switch, service or Game DVR write is counted and named ("SysMain: Access is
  denied. Restart as administrator and try again."), a run that changed nothing
  no longer stores itself as on, and a refused service change during apply drops
  its record again because nothing was changed. An older restore reports the
  services it could not put back and stays on so the retry is still offered.
- **The Game Booster header counts the profile's own rows.** It read "partly
  on" whenever the three advanced rows were off, which the profile never sets
  anyway; it now reads ON when the system row and all four profile tweaks are
  on, and the counts say "4/4 profile tweaks - 0/3 advanced". The profile row
  also stops claiming background services are parked when none were.

## [0.2.1] - 2026-10-24

Scrollable storage lists, tick-box multi-select on Services and Task manager,
every Game Booster row controllable from the page, and a release step that keeps
only the current package.

### Added

- **Publishing a version clears out the old package.** After the new
  `release\Zanphar-WinSuite-v<version>\` folder, its zip and its `.zip.sha256`
  are written, `make_release.ps1` removes every other `Zanphar-WinSuite-v*`
  package, archive and checksum from that folder, so `release\` never holds two
  versions to choose between. The match is limited to that name pattern (other
  files in `release\` are untouched) and this version's own three artefacts are
  never removed. Before anything is staged, every older package is probed for
  open files; one in use - an archive open in Explorer or a scanner reading it -
  stops the run there, names the busy file and writes nothing, so a doomed
  release cannot leave the folder half updated. The pruning then runs last, so a
  failed build or packaging step still leaves the previous release untouched;
  if a lock appears between the probe and the removal, the run fails and names
  the stuck items instead of only warning. The new package is left complete, so
  once the old file is freed a rerun only has to prune. `release.bat` reports
  what it pruned and exits non-zero on a stuck file.
- **CI refuses a release folder holding two versions.** The build job now has a
  step after packaging that reads `VERSION` and requires `release\` to hold
  exactly one package folder, one archive and one checksum, all named after that
  version. Two packages, a missing `release\`, an archive left over from another
  version or a name that disagrees with `VERSION` all fail the job before the
  artifacts are uploaded, so the publish job can never attach a stale archive.
- **CI also proves the packaged documents are the repository's.** A companion
  step hashes `README.md`, `CHANGES.md` and `LICENSE.md` in the staged package
  and in the repository root and fails unless every pair is byte-identical. A
  document edited after packaging, or changed inside the staging folder, can no
  longer ship an archive that describes a different release from the source.
- **Every connected storage device is reachable.** The **Storage** card on the
  Dashboard and the **Storage devices** card on System both list every volume
  Windows reports - fixed, removable, external, optical, network and RAM disks,
  with the bus, model, file system, free space and usage bar - and now scroll
  through all of them instead of stopping at the first screenful ("4 of 6"). The
  wheel moves the list while the pointer is on the page, the scroll snaps to a
  whole volume so no row is left half drawn, a slim bar shows the position, and
  the header reads "6 volumes - scroll for all" while there is more to reach.
- **Ticks for bulk service and process actions.** Services and Task manager rows
  now carry a tick box: press rows to add them to the set, press again to take a
  tick off, use **Select all** (the rows the search box is showing) or **Clear
  ticks**. **Start**/**Stop** on Services and **End task**/**Force end** on the
  task list then act on every ticked row that needs it, in list order, with one
  administrator prompt and one summary - "Started 3 services", "2 of 4
  processes ended; 1 refused". The footer counts the ticks, the highlight still
  drives the start-type chips and the affinity card, and with no ticks the
  buttons fall back to the highlighted row exactly as before. Ticks are held by
  service name and by pid, so a refresh that reorders either list keeps them on
  the right rows; a filter that hides a ticked row drops that tick, so a bulk
  action never touches a row that is out of sight. `--selftest` gained
  `bulk ticks follow services and processes by identity`, which ticks one real
  service and one real process and checks that no other row reads as ticked.
- **Every Game Booster row is controllable from the page.** A row whose value
  was already on when WinSuite found it used to read **Set elsewhere** and could
  not be touched here. It now offers **Turn off**: the current value is
  journalled with its type and then removed, so Windows is back at its own
  default without guessing an "off" value, and **Revert** still restores exactly
  what the other tool had set.
- **A latency check on Internet & networking**, so the tweaks' effect is a number
  rather than a claim. A **Latency check** card sits under the profile summary with
  a host field (1.1.1.1 to start), a button and **Reset**; Enter in the field starts
  the measurement too. The button reads **Measure before** until a baseline exists,
  then **Measure after**; each press sends four ICMP echoes with a one-second
  timeout from a worker thread and reports
  back by message, so the window never freezes for the length of a timeout. The
  card shows both readings as "18 ms avg (12-25 ms), 0% loss" and, once there are
  two, the difference - "3 ms lower on average", amber when it rose, and it adds
  that packet loss moved too when it did. Any name or IPv4 address can be measured
  (it resolves over IPv4 first); a host that does not resolve or does not answer
  gets a plain error instead of a reading, and editing the host drops both readings
  because they belong to the host they were taken against. The page now scrolls, so
  the latency card and the four tweak rows keep their full explanations instead of
  being squeezed; the wheel moves it and a slim bar shows the position. Edited text
  is held in the window only, so a restart starts a fresh comparison. Engine:
  `MeasureLatency` in `src/netgame.cpp`; `--selftest` gained
  `latency check pings a host and clamps the sample count`, which measures 127.0.0.1
  (so no adapter or internet is needed) and checks the sample count is clamped to
  one echo when asked for zero - skipped, not failed, where ICMP is refused.
- **Every Internet & networking row is controllable.** A value that was already in
  gaming shape when WinSuite found it used to read **Set elsewhere** and could not be
  changed from the page. The row now offers **Turn off**: the value that is there is
  journalled with its type and then removed, so Windows is back at its own default
  without guessing at an "off" value, and **Revert** puts back exactly what the other
  tool had set - per row or through **Revert all**. Turning off a row that has nothing
  left to remove says it is already at the Windows default, and a partial refusal names
  the value that was left alone. Measured with a harness on HKCU test keys: a Games row
  set elsewhere reports applied without a journal, turn off removes all four values,
  revert restores all four (GPU Priority 8, Priority 6, High, High), and the same holds
  for Nagle's two values on an adapter.
- **Applying the profile takes the latency baseline.** Pressing **Apply profile** with
  no reading yet now measures the baseline first and puts the tweaks on by itself once
  that reading lands, so the comparison costs one press instead of two and the card is
  left reading **Measure after**. The reading is taken while nothing has been written -
  an echo collected after the values changed would be an after-reading wearing the wrong
  label - and a measurement already in flight is the one that lands, never a second one.
  **Apply profile** and **Revert all** dim while it runs, so a second press cannot queue
  another apply or undo a profile that has not gone on yet. A ping that gets no reply
  does not cancel the press: the card keeps the error ("No reply from 192.0.2.1 - the
  host may be down or block pings.") and the profile goes on regardless, and a baseline
  already in hand is never replaced.
- **Every adapter on the DNS page is reachable.** The adapter list used to stop at the
  bottom of the window with the rest unreachable. It now scrolls: the wheel moves it by
  a whole card, it ends on a card edge so no adapter is left half drawn, a slim bar on
  the right shows the position, and the button row reads "10 adapters - scroll for all"
  while there is more to see (just "10 adapters" once everything fits). Every adapter
  Windows reports is listed in that list, including the Hyper-V and kernel-debugger ones.
- **Search boxes on Services and Task manager.** A field above each list narrows it
  as you type: on Services it matches the display name or the service name, on the
  task list the process name, both case-insensitively and anywhere in the name
  ("spool" finds **Print Spooler**). The field takes the keyboard when clicked and
  shows a caret; **×** clears the text without leaving it, Escape clears it and a
  second Escape gives the keyboard back, and clicking anywhere else takes the
  caret away. The list, its scroll position and the footer follow the match set -
  "1 of 310 services match 'spool'" - with an empty-list line when nothing
  matches, and a selection the filter hides is dropped so Start/Stop and the task
  card never act on a row that is out of sight. `--selftest` gained
  `the list search boxes narrow by name, case-insensitively`, which runs the shared
  matcher over this PC's real service and process names.
- The Memory page's **Regular clean** and **Deep clean** confirmations are now
  controlled from Settings: **Ask before Regular clean** and **Ask before Deep
  clean**, both **on** by default and stored as `ConfirmRegular` / `ConfirmDeep` in
  the `[Memory]` section of `settings.ini`. With a prompt switched off the clean
  runs straight away; the Deep clean's administrator prompt still appears when
  elevation is what is missing, because the clean cannot run without it. The
  Settings page now scrolls when the window is short, with the same clipped-hit
  handling as the other lists, so the new section does not push the licence off
  screen. `--selftest` flips both choices, reloads them and puts the user's own
  choice back.
- A **Game Booster** page, which absorbs the old **Boost** page: the System Turbo
  profile is now its first row. **Apply profile** applies that profile (High
  performance plan, parked background services, no Game DVR) plus the four
  tried-and-true rows - **Game Mode on**, **mouse acceleration off** (1:1 pointer,
  pushed into the running session with `SPI_SETMOUSE`), **fullscreen optimisations
  off**, and **window animations and transparency off** (`SPI_SETANIMATION` too).
  **Revert all** puts back everything WinSuite saved. The three advanced rows -
  **hardware-accelerated GPU scheduling**, **power throttling off** and **Win32
  priority separation 38** - are individual-only, never part of the profile, and
  carry their caveats and restart requirement in the row. Every value, including an
  absent one, is journalled with its type under
  `HKCU\Software\ZanpharsWinSuite\GameBooster`, so a revert restores exactly what
  was there; a value another tool already set is marked **Set elsewhere** and is not
  claimed or overwritten. The list scrolls with a position bar and keeps every row's
  explanation on screen. Engine: `src/gameboost.cpp`; `--selftest` checks the
  grouping (four profile rows, three advanced) and round-trips a string value through
  the journal, type included.
- An **Internet & networking** page with four tried-and-true gaming tweaks:
  **Nagle's algorithm off** (`TcpAckFrequency` and `TCPNoDelay` on every
  adapter that holds an IPv4 configuration), **network throttling off**
  (`NetworkThrottlingIndex=0xFFFFFFFF`, `SystemResponsiveness=0`), the **Games
  task profile** (GPU priority 8, priority 6, high scheduling class and SFIO
  priority) and **QoS reservation off** (`NonBestEffortLimit=0`). Each row has
  its own switch, and **Apply profile** / **Revert all** act over the same
  per-tweak calls. Before a value is written its data and type are saved in
  `HKCU\Software\ZanpharsWinSuite\NetGaming` (an absent value is recorded as
  absent), so a revert restores exactly what was there - including removing a
  value this suite created - instead of guessing at a Windows default. Values
  another tool already set are shown as on, with no saved original, and are
  never overwritten by a revert. The page reports the adapter and value counts
  it acts on, needs administrator rights (it offers the restart), and says that
  a restart is what makes Windows re-read the new values. Engine:
  `src/netgame.cpp`; `--selftest` checks the catalog and round-trips a value
  through the journal under `HKCU` without touching a real network setting.
- A **Services** page: every installed Windows service with its display name,
  SCM name, binary, start type and state in a list that scrolls, with a
  `Windows` pill on the entries Windows owns. **Start**
  and **Stop** act on the selected row; four chips set its start type
  (**Automatic**, **Automatic (delayed)**, **Manual**, **Disabled**) and apply
  at once. **New…**, **Edit…** and **Delete** create, change or remove the
  entry through a small form (name, display name, binary path, description);
  they are offered only for entries whose binary lives outside `%SystemRoot%`,
  so Windows components can be started, stopped and re-typed but never edited
  or deleted from here. The engine is `src/svcctrl.cpp`; every action reports
  what the service manager said and offers the administrator restart when
  rights are the only thing missing.
- A **Task manager** page listing every process on the PC with PID, account,
  affinity, priority and memory. **End task** posts WM_CLOSE to the process's
  windows, **Force end** terminates it behind a warning, and **New task…**
  starts a program or command line in the suite's session. The selected
  process gets one chip per logical CPU plus **Apply cores**, and the six
  priority classes plus **Apply class** (Real time asks first, because it sits
  above the shell and the mouse). **Remember these for this program, at
  sign-in too** saves the pair as a rule for that executable: it is applied to
  the matching processes at once, written to the `[TaskRules]` section of
  `settings.ini`, and enforced again at sign-in by a new `bin\WinSuiteAgent.exe`
  that the suite puts in the per-user `Run` key the first time a rule is saved;
  **Forget the saved rule** removes the rule and, when the last one goes, the
  `Run` entry too. The list holds still while a process is selected, so rows
  appearing and leaving cannot move it out from under the buttons, and it
  resumes following the PC as soon as nothing is selected. Console programs
  are not sent a Ctrl+Break: that console includes the suite, and asking it
  would close the suite itself - they are told to use Force end instead.
- The sidebar scrolls when its entries no longer fit: the wheel over it moves
  the list, a slim bar shows the position, and the rows only shrink to the
  point where the labels still read. The added pages took the count to
  fourteen, which is what made this necessary on a short window.
- A **Settings** page in the sidebar with two window options, both off until
  chosen and stored in a `[Window]` section of `settings.ini`
  (`CloseToTray`, `MinimizeToTray`, read at launch). **Close to system tray**
  turns the X button into a hide: the window leaves the screen and a
  notification-area icon appears, a left click or double click on it brings the
  window back, and its right-click menu offers **Open** and **Exit**, the only
  way to quit while the window is hidden. **Minimize to system tray** does the
  same for the minimise button instead of sending the window to the taskbar.
  If the notification area refuses the icon the window stays where it is and a
  close still exits, so the app can never hide itself with no way back, and an
  Explorer restart puts the icon back through the `TaskbarCreated` broadcast.
- The Settings page carries the licence: `Copyright (c) 2026 Charles McDonald.`
  and `This software is licensed under the terms of the BSDv3-Clause License
  Agreement.` with a **Read the licence** button that opens the full BSD
  3-Clause text read-only. The agreement covers the window in its own view -
  title, a line saying it is read only, the whole text wrapped at the window
  width, wheel scrolling when the window is too short for it, and Close (or
  Escape) to return. While it is up every other hit is dropped, so nothing
  behind it can be pressed; the text is compiled into the program
  (`kLicenseText`) and `--selftest` checks the holder, the grant, the third
  clause and the disclaimer are all still in it.
- Cleanup gained six more targets: **Your Downloads folder**, **Windows
  Update downloads** (`SoftwareDistribution\Download`), Windows Prefetch, live
  kernel reports, Windows minidumps and system error reports. The Downloads
  row is the only one that holds personal files: it arrives unticked, carries
  a `your files` pill that turns red once it is ticked, and the page's header
  and footer now say so instead of promising the folder is never touched.
  The other new rows are normal opt-out caches; the system ones are marked
  `admin` like Windows Temp already was.
- `release.bat /build` — one command that rebuilds the suite cleanly and then
  packages it: it runs `build_all.bat /clean` (which deletes `bin\` and the
  staging area first) and aborts before packaging if that build fails, so the
  archive can only contain binaries produced by that run. Without `/build` the
  script still packages the existing `bin\` as-is, and `/version X.Y.Z`
  combines with either form.
- `.github\workflows\build.yml` — CI on `windows-latest`: installs the MSYS2
  UCRT64 toolchain (`mingw-w64-ucrt-x86_64-gcc` via `msys2/setup-msys2@v2`),
  exports `MINGW_BIN` from the action's `msys2-location` output, then runs the
  same command a maintainer would use, `release.bat /build`, so every push
  builds cleanly and packages in one step. It verifies the outputs and
  uploads two 30-day artifacts: `bin\*.exe` as `Zanphar-WinSuite-bin-x64`
  and `release\*.zip` plus its `.sha256` as `Zanphar-WinSuite-release-zip`.
  Triggers on branch pushes, pull requests and manual dispatch.
- `build.yml` release job — pushing a `v*` tag now publishes: after the build
  job succeeds it downloads the uploaded archive artifact (no second build and
  no second packaging), checks the tag against `VERSION`, re-hashes the
  archive against its `.sha256` and creates the GitHub release with
  `gh release create --verify-tag --generate-notes`, attaching the ZIP and
  `.sha256`. Re-runs re-upload with `--clobber`; a suffixed tag such as
  `v0.3.0-rc1` becomes a prerelease. The job holds `contents: write` and is
  skipped for pull requests, so the build job stays read-only. Tags other than
  `v*` no longer trigger the workflow; branch pushes and pull requests are
  unchanged otherwise.
- `build.yml` release notes — the publish job takes the version's section out
  of `CHANGES.md` (from `## [<version>]` down to the next `##` heading,
  heading excluded) and writes it to `release-notes.md` in UTF-8 without BOM,
  which `gh release create --notes-file` publishes as the release body.
  Auto-generated notes are no longer used, so the release page and the
  changelog cannot drift apart. A missing section, a section with no body, or
  a version that only appears in prose fails the job with the version in the
  message. On a re-run the body is refreshed with `gh release edit
  --notes-file` alongside the re-uploaded assets.
- `tools\verify_binaries.ps1` — standalone self-contained check: reads every
  executable's import table with `objdump` and fails on any DLL outside an
  allowlist of Windows-provided ones (plus the `api-ms-win-*` API sets). It is
  independent of the check `build_all.bat` performs while building.
- README: "Continuous integration" section, `/build` in Quick start and in
  release packaging, plus CI and verification entries in the repository tree.

- `build.yml` manual publishing — `workflow_dispatch` now takes a `tag`
  input: `gh workflow run build.yml -f tag=v0.2.0` (or **Run workflow** in
  the browser) checks that tag out in both jobs, builds and packages it, then
  publishes or refreshes its release. An empty input keeps the previous
  behaviour: build and package the selected ref, publish nothing. The publish
  job starts on a tag ref or on a manual run that named one, and reads the
  tag from the input instead of `github.ref_name`, which is the selected
  branch for a manual run.

- Smart RAM reports a clean on the card instead of in a notification: a line
  under `CLEAN NOW` reads `1.24 GB FREED`, or `NOTHING TO FREE` when the
  release was under 1 MB, and stays until the next clean or restart. It is
  green at 96 MB and above, dim below that. The card grew from 316×412 to
  316×436 so the band has room, and the clean-finished message carries the
  released byte count again.

- The suite is the primary interface: three new pages carry the four programs'
  engines, so nothing has to be launched separately.
  - **Browsers** (Browser Cleaner): Chrome, Edge, Brave, Vivaldi, Opera, Opera
    GX and Firefox. Each browser's history, cookies, autofill, caches and
    session data are measured, SQLite sidecars included, before anything is
    deleted; a running browser is detected, greyed out and skipped, and its
    windows can be closed from the page first.
  - **DNS** (DNS Protect): every configured adapter with its live resolver,
    five providers applied to all adapters at once, system DNS restored, and a
    1.5 s watchdog thread that rewrites drift and counts what it reverted. The
    applied servers, provider and watchdog state persist in `settings.ini`, so
    the watchdog resumes on the next launch.
  - **Boost** (System Turbo): High performance plan, seven parkable services
    stopped and set to manual, Game DVR recording off, every original value
    journalled under `HKCU\Software\ZanpharsWinSuite\Turbo` so Restore puts it
    back.
  - **Memory** gained Smart RAM's settings: automatic clean at 65/75/85/95 %,
    clean when the machine has been idle for five minutes, and clear the
    clipboard after a clean.
- `src\browsers.cpp`, `src\dns.cpp`, `src\boost.cpp`, `src\prefs.cpp` and the
  shared `src\extras.h`; `build.bat` compiles them and adds `-lpowrprof`.
- `--selftest` gained five read-only checks for the new engines: the browser
  catalog, a real trace measurement, adapter enumeration, the active power
  plan and a memory-options round-trip.

### Changed

- Every executable's **Publisher** now reads **Charles McDonald**, matching its
  copyright line and the BSD 3-Clause licence. The `CompanyName` string in each
  version block was the last place the old branding survived - `Zanphar` in the
  suite and `WinSuiteAgent`, `Open Source` in `BrowserCleaner`, `Smart RAM`, and
  `DNS Protect project` / `System Turbo project` - so `bin\\*.exe` had one owner in
  the copyright and another in the publisher. All six `.rc` files, and therefore
  Explorer's Details tab, Task Manager and the release `MANIFEST.txt`, now name
  the same person. Existing binaries were rebuilt and the release package was
  restaged from them.
- The product opens at **1024 x 768** instead of 1300 x 900: the window is
  centred at that size on the screen it starts on, and `--shot` renders every
  page at the same size so the reference images match what the app opens with.
  Nothing is clipped at that size - the sidebar tightens its rows to keep all
  thirteen entries visible - and the window can still be dragged between its
  tracked minimum of 980 x 640 and the desktop size.
- The suite launcher now lives in `bin\` beside the four programs instead of
  `build\`: `build.bat` links `bin\ZanpharsWinSuite.exe` and runs its
  `--selftest` from that path, and `tools\check-icon.ps1` reads the icon from
  it too. `build\` keeps only the compiled resource object (`app.res.o`) and
  an old service-test log. Because `release.bat` stages `bin\*.exe` and CI
  uploads the same glob, the launcher now travels with the packaged set.
- `build_all.bat` builds the suite launcher as its last build step: it calls
  `build.bat`, which links `bin\ZanpharsWinSuite.exe` and runs its
  `--selftest`, and a failing self-test fails the run. `build_all.bat /clean`
  - and therefore `release.bat /build` and CI - can no longer produce a
  `bin\` without a tested launcher in it, which also retires the earlier note
  that `/clean` wiped `bin\` and left the launcher missing until someone ran
  `build.bat` by hand.
- The project folders and binaries lost their `Clone` suffix:
  `BrowserCleanerClone\`, `DNSProtectClone\` and `SystemTurboClone\` are now
  `BrowserCleaner\`, `DNSProtect\` and `SystemTurbo\`, and the outputs are
  `bin\BrowserCleaner.exe`, `bin\DNSProtect.exe`, `bin\SmartRAM.exe` and
  `bin\SystemTurbo.exe`. The recipe table in `build_all.bat`, both
  `build_mingw.bat` scripts, the `.gitignore` entries, the version resources
  (`OriginalFilename`), the manifest identities, the single-instance mutex
  names, and the README, LICENSE and changelog paths were updated to match,
  and the fallback rule that stripped a trailing `Clone` from a folder name is
  gone.
- `build_all.bat` builds projects that have no recipe row from an inlined loop
  instead of `call :build_auto`. That label call sat after the
  `:build_project` calls and failed with "cannot find the batch label
  specified - build_auto" on every run, so the documented automatic fallback
  never actually ran.

### Changed

- One licence for the whole product: **BSD 3-Clause**, copyright 2026 Charles
  McDonald - the same agreement the Settings page displays. `DNSProtect` and
  `SystemTurbo` were still shipping MIT files; both are now BSD 3-Clause copies
  of the suite's terms, their READMEs say so, and their version resources report
  the BSD licence instead of MIT. The suite exe's `LegalCopyright` no longer
  claims "All rights reserved", and `SmartRAM` and `BrowserCleaner` carry the
  same line, so every binary in `bin\` states the licence it ships under. The
  README's licence section and repository tree follow it, and every copyright
  line names Charles McDonald rather than the `Zanphar` brand. `WinSuiteAgent.exe`
  had no version block at all - no copyright, description or product name - so it
  now carries `src\agent.rc` with the same identity as the suite, and no binary in
  `bin\` is left without a copyright line.
  `release\Zanphar-WinSuite-v0.2.0` was repackaged from the rebuilt binaries so
  its licence, README and exe metadata match the rest of the tree.
- The Cleanup page has an **Order** switch over the list: **Biggest first**
  (the default) or **Built-in order**. It sits at the right end of the summary
  line under the buttons, the mode in use is filled, and pressing the other one
  reorders at once and drops the scroll back to the top so the new order is
  read from its first row. The choice is remembered between launches under
  `[CleanupView] Order` in the suite's settings, in its own section so **Reset
  to defaults** - which drops the stored ticks - leaves the order alone; a
  missing key means the default, biggest first. It touches no figures and no
  ticks. The status line moved to its own line under the buttons to make room,
  where it ellipsizes instead of running under the switch.
- The Cleanup list is ordered by measured size: the biggest folder sits at the
  top. The order is worked out when a measuring pass lands, when a stopped pass
  is acknowledged, and when the post-clean refresh arrives - never in the middle
  of a pass - so rows do not jump under the cursor while figures fill in. A row
  a stopped pass never reached sorts by the earlier figure it still shows; rows
  with nothing to compare (not on disk, or never measured and no earlier
  figure) stay below the figures in their built-in order, equal sizes keep the
  built-in order, and the status line's `n of m` still counts the pass. The
  list keeps its built-in order internally: the size order is a separate list
  of row positions, so the scan results, the bulk controls and the stored
  choices still address rows by identity and a click toggles the row it was
  drawn under. Your Downloads folder can top the list when it is the biggest
  target on the page; it still arrives unticked and its warning still guards
  the clean.
- Rows a stopped pass never reached keep their earlier figures instead of
  showing a dash: the figure stays on screen in the dim text colour, the file
  count stays with it, and a neutral `earlier` pill sits beside the name, so a
  stale number is never mistaken for a fresh one. Rows that had nothing
  measured yet still read `-` / `not measured`. The earlier figure remains out
  of the selected totals and out of the clean, because it describes a pass the
  user stopped; **Scan now** replaces it with a fresh measurement.
- A running Cleanup scan can be stopped: while a pass runs the **Scan now**
  button becomes **Cancel** in the same place, reads **Stopping...** until the
  worker acknowledges, and the walk ends at the next file boundary -
  `ScanCleanGroup()` takes an optional cancel flag that `ScanTree()` checks per
  entry, so a 36,000-file folder stops in milliseconds rather than only
  between folders. Rows already measured keep their figures; the interrupted
  row and everything after it stay unmeasured and paint `-` / `not measured`
  instead of a zero that reads like an empty folder, and a banner says
  `Scan stopped - 10 of 11 folders measured.` A stopped pass is not restarted
  by the once-a-second self-measure or by the quiet refresher; **Scan now**
  finishes it, and leaving the page and returning counts as a fresh visit.
  Unmeasured rows are left out of the selected totals and out of the clean
  itself, so nothing is cleaned behind a figure the user never saw.
- The Cleanup page shows measurement row by row instead of one **Scanning...**
  label: a scan marks every row unmeasured when it starts, the worker posts
  `WM_APP_SCAN_PROGRESS` as each target finishes, and the page takes that row's
  figures (`bytes`, `files`, `error`, `exists`, `scanned`) while leaving the
  ticks alone. Until its turn a row reads `Measuring...` in the size column and
  the status line reads `Measuring folders... 10 of 11`. The pass walks the
  list back to front, because it is built with the heavy user caches first -
  so the quick rows land at once and the wait is visibly spent on the folder
  that actually takes the time, instead of all eleven sitting at zero.
- **Select all** and **Clear all** sit beside **Clean selected** on the Cleanup
  page. Select all ticks every target that is actually present but never the
  user's own folder, so a bulk press cannot arm Downloads; Clear all clears
  every row. Both are remembered like a row click (both go through
  `ApplyBulkCleanSelection()`), and the selection line and the clean button
  follow at once - after Clear all the button is disabled because nothing is
  ticked.
- A scan no longer replaces the ticks made while it ran: `WM_APP_SCAN_DONE`
  now feeds its measured rows through the same by-id tick carry the quiet
  scan and the post-clean refresh already use, so a **Select all**, a **Clear
  all** or a single row clicked during the measurement stays as you left it
  instead of snapping back to the snapshot taken when the scan started. With
  the page measuring itself on first open this was easy to hit.
- A **Reset to defaults** link at the foot of the Cleanup page drops the
  remembered ticks: it removes the `[Cleanup]` section from `settings.ini` and
  puts every row back to the selection `BuildCleanGroups()` asks for - caches
  ticked, the user's own folder unticked - without disturbing the figures
  already measured, so nothing has to be scanned again. It is a quiet text
  link rather than a button because it only changes which boxes are ticked,
  and the selection line above it updates as soon as it is clicked.
- The Cleanup page remembers which rows you ticked: a `[Cleanup]` section in the
  same `settings.ini` the DNS, memory and elevation preferences use carries one
  `1`/`0` per target id, written when a row is clicked and read once at
  launch - so a cache you opted out of stays out next time, and ticking it
  again undoes that. The user's own folder is the exception both ways: it is
  never written and never restored ticked, so Downloads cannot come back
  armed. The page clearing its ticks after a clean is not saved as a choice,
  which keeps an everyday clean starting from the usual defaults.
- Ticking **Your Downloads folder** now asks before cleaning: a modal warning
  names the row, its size and file count, and says the files go to the Recycle
  Bin, with **No** as the default button so a stray Enter cancels. Caches
  never prompt, so an everyday clean is still one click.
  `PersonalCleanPrompt()` builds the text from the same rows the page shows
  (a personal row with nothing to remove is left out) and
  `ConfirmPersonalClean()` shows it from `StartClean()` before any worker
  starts, so cancelling leaves the page idle and nothing half-cleaned.
- The Cleanup page measures itself the first time it is opened: opening it
  with nothing measured yet starts the same scan **Scan now** runs, so the
  page never sits on "Nothing scanned yet" waiting for a button press. The
  rule, `EnsureCleanupMeasured()`, is also checked once a second while the
  page is open, which covers the case where another job - a large-file scan,
  a memory clean - still holds the worker when the page appears: the
  measurement then starts as soon as the worker is free instead of being
  dropped. A page that already has figures is never re-measured on a visit,
  because the quiet scan keeps those fresh and **Scan now** is still there for
  a deliberate re-measure.
- Cleaning your **Downloads** folder is now reversible: a row flagged as
  personal is handed to the Recycle Bin instead of being deleted, and whole
  folders cross over as folders, so `Downloads\Project` comes back with its
  contents. `CleanGroupContents()` in `src\core.cpp` picks the path -
  `RecycleUnderRoot()` for personal rows, `DeleteUnderRoot()` for caches - and
  `StartClean` now passes the rows themselves instead of bare paths, so the
  choice follows the row rather than the button. Items go to the shell in
  batches (64 paths or 30,000 characters per call) and each is checked
  afterwards, because a file another program holds open stays put and is
  reported as left alone. `CleanResult` carries `bytesRecycled` separately
  from `bytesFreed`, so the banner reads `Moved 357 MB to the Recycle Bin
  from 11 files` rather than claiming space came back, and the page's footer
  says those files sit in the bin until it is emptied. Caches keep being
  deleted outright; the Recycle Bin itself is never emptied.
- Cleaning now finishes in an idle, scannable state, and the item list is
  refreshed right behind it: instead of starting a second full scan (which
  kept the Scan button on `Scanning...` and disabled until every folder had
  been measured again), the page rebuilds the list from the targets currently
  on disk and re-measures every one of them in the background, without taking
  the page busy. Scan is usable the moment cleaning ends, the rows show real
  leftovers (including anything a locked file kept), and ticks the user makes
  while the refresh runs are kept - rows that were just cleaned stay unticked.
  The refresh runs whether or not the Cleanup page is open when the clean
  finishes, and the `Freed ...` banner stays put while the rows behind it are
  replaced, so a cache that regrows immediately (Windows Update recreates its
  download folder) is shown at its real size instead of the old figure.
- `--selftest` no longer fails on a machine that lacks what it inspects: with
  no configured network adapter, or no third-party auto-start service, those
  two checks print `[SKIP]` with the reason instead of `[FAIL]`. Every other
  check still fails as before, so a real defect is never hidden - this is what
  lets CI run the full self-test on a clean runner.
- `--selftest` ends with one machine-readable line carrying every count:
  `RESULT: 28 passed, 0 skipped, 0 failed.` It replaces the previous
  prose-only closing lines ("all checks passed", "N check(s) failed"), so a
  CI log can be grepped for `RESULT:` and read at a glance instead of counting
  `[PASS]` lines. The exit code is unchanged: `0` only when nothing failed.

### Removed

- Smart RAM's notifications: the tray balloon and in-card toast shown after a
  clean are gone, and so is the balloon shown when the card is closed to the
  tray. `TrayIcon::Balloon`, the toast state and its drawing code were
  removed; the clean-finished message no longer carries a freed byte count.
  The tray icon, its tooltip, the settings and the cleaning itself are
  unchanged — a clean now only refreshes the card.

### Fixed

- Content no longer overlaps on a display that is not at 100%: the window opened
  at 1024 x 768 **physical** pixels while the app draws in DPI-scaled units, so
  at 125% the layout had only 819 x 614 to work with and the Game Booster heading
  ran under **Apply profile**, the Storage card's drive names and sizes sat on
  top of each other, and the Cleanup subtitle ran past the window edge. The
  opening size, and the 980 x 640 tracked minimum, are now multiplied by the
  display scale (`SystemScale`, with `GetDpiForSystem` and a `GetDeviceCaps`
  fallback), and the window is capped to the work area on a screen too small for
  that. Moving the window to a monitor with a different scale now follows the
  rectangle Windows suggests in `WM_DPICHANGED`, so the layout is re-sized
  instead of squeezed.
- Settings option rows grew taller than their fixed 72 px at a narrow window, so
  the last line of **Ask before Deep clean** was cut through the middle against
  the card edge. A row's height is now measured from its own explanation at the
  width it will be drawn in (`OptionRowHeight`, using `WrappedHeight`), so the
  card grows and every line stays readable; the page's scroll range follows.
- A Cleanup row scrolled above the list kept its full card as a click target,
  so its invisible top silently took presses meant for the toolbar over it -
  with the list scrolled, pressing the new Order switch or **Select all** could
  tick a row that was out of sight instead. Row hits are now clipped to the
  visible list, so only the part of a row that is on screen answers a click.
- Boost's Game DVR tweak never ran: `GameDVR_Enabled` and
  `AppCaptureEnabled` are `REG_DWORD` values, and the suite's shared registry
  helpers read and write strings only, so the boost could not see either value
  and silently skipped it. `src\boost.cpp` now carries its own `ReadDword` /
  `WriteDword` (64-bit registry view) for those two values, and `--selftest`
  gained `BoostDwordRoundTrip`, which writes, reads back and removes a
  `REG_DWORD` in the boost journal.

### Verified

- The overlap fix was checked at both scales. A running app still opens at
  1024 x 768 on this 96 dpi desktop (client 1008 x 729) and every page renders
  as before; the same layout painted at 1280 x 960 with a 1.25 scale - what a
  125% display now opens with - showed the Game Booster heading clear of
  **Apply profile**, both Storage columns separate, the Cleanup subtitle on one
  line, and the Settings rows fully drawn. Settings at the 980 x 640 minimum
  shows all four explanations in full, with the ABOUT card reached by wheel
  scrolling and the position bar on the right.
- The Settings page was driven in a running app with a throwaway
  `LOCALAPPDATA`: pressing **Close to system tray** wrote `[Window]
  CloseToTray=1`; a `WM_CLOSE` hid the window while the process stayed alive,
  and `Shell_NotifyIconGetRect` found the tray icon registered with the shell;
  the icon's left-click callback brought the window back and the icon was gone
  again; **Minimize to system tray** hid the window on `SC_MINIMIZE` and the
  same callback restored it; with both switches off a close ended the process.
  A restart with both on reopened the page with both rows ticked and `On`.
- The agreement was opened from that page and left with Escape, and a render at
  760x420 showed the text scrolling to its end and stopping there (max 160 px).
  A scratch console build painted the page and the view and read the hits back:
  both option rows, the licence button and the Settings nav entry present, no
  Cleanup rows on the page, and exactly one hit - Close - while the agreement
  covers the window. `RESULT: ok`, exit `0`.
- `build.bat` and `--selftest` after the Settings page: exit `0`, 39 checks,
  `RESULT: 39 passed, 0 skipped, 0 failed.`, including `[PASS] tray options
  default to off and are remembered -> fresh profile off: yes, close-to-tray
  kept: yes, minimize left off: yes, flipped: yes` and `[PASS] licence view
  carries the full BSD 3-Clause text -> copyright holder: yes, grant clause:
  yes, third clause: yes, disclaimer: yes`.
- The Order switch was pressed in a running app: the page opened on **Biggest
  first** with Downloads (357 MB) above temporary files (58.3 MB); **Built-in
  order** reordered the list to the built-in one (temporary files first) and
  lit its own chip; **Biggest first** put the sizes back. A row clicked while
  the list was size-ordered ticked the row under the pointer - temporary files,
  which sits below Downloads - and left Downloads unticked, so the reordered
  list still toggles what it shows. Scrolled to the bottom, the switch still
  answered and returned the list to its first row, and a click on a row that
  was partly above the viewport went to the row, not to the toolbar over it. A
  scratch console build painted both modes at 1280 and at the 980 px minimum and
  read the hits back: `BACD` against `ABCD` built-in, one hit per chip, chips
  side by side, and with the list scrolled no row hit reaches the switch (first
  row top 196 = list top). `RESULT: ok`, exit `0`; the harness, its images and
  the app's scratch settings folder were deleted.
- The remembered order was checked across a real restart with a throwaway
  `LOCALAPPDATA`: **Built-in order** was pressed, the settings file then held
  `[CleanupView] Order=builtin`, the app was closed and reopened, and the
  Cleanup page came up in **Built-in order** with the switch filled on that
  side. `build.bat` and `--selftest` after it: exit `0`, 37 checks,
  `RESULT: 37 passed, 0 skipped, 0 failed.`, including `[PASS] cleanup order
  choice is remembered across launches -> defaults to biggest first: yes,
  built-in remembered: yes, survives a tick reset: yes, back to biggest first:
  yes` and `[PASS] cleanup rows sort by size, or in built-in order on request
  -> by size: bdcaefh, every row once: yes, built-in order: yes, switched
  back: yes`.
- The row order was checked at both ends. `--selftest` builds a mixed list
  (measured, earlier figure, measured and empty, never measured, not on disk)
  and asserts the drawn order is `bdcaefh` - biggest first, the earlier figure
  in its place, the rest in built-in order, every row exactly once. A scratch
  console build painted the real Cleanup page into a memory DC and read the
  hits back: the rows came out `BACD` (biggest first) with every hit naming a
  real row, a click on the top row toggled the group its id names rather than
  the group at that screen position, and a stale order (list length changed)
  fell back to the list it had instead of skipping or repeating a row.
  `RESULT: ok`, exit `0`. A `--shot` render of the real page showed 357 MB for
  Downloads, then 59.3 MB temporary files, then the measured-empty rows; the
  harness, its binary and those images were deleted.
- The earlier-figure state was driven through the real handlers by a scratch
  console build: a complete pass measured every row, a second pass was
  cancelled while the temp folder was being walked, and that row kept
  `59.3 MB` / `35 files` with `scanned=0` while the banner read `Scan stopped -
  10 of 11 folders measured.` The rendered page showed the row dimmed with the
  `earlier` pill, its neighbours at fresh figures; **Scan now** then replaced
  it and all eleven rows were measured. `RESULT: ok`, exit `0`; the harness and
  its images were deleted.
- `build.bat` and `--selftest` after the earlier-figure change: exit `0`, 35
  checks, `RESULT: 35 passed, 0 skipped, 0 failed.`
- Cancel, driven through the real handlers by a scratch console build: a pass
  was started, its first ten quick rows landed, and **Cancel** was pressed
  while the big temp folder was being walked. The button read `Stopping...`;
  the finish reported `Scan stopped - 10 of 11 folders measured.`, the temp row
  went from `Measuring...` to `-` / `not measured`, and the ten measured rows
  kept their figures. `EnsureCleanupMeasured()` then left the pass stopped,
  **Scan now** restarted it and measured all eleven, and a Cancel with nothing
  running did nothing. `RESULT: ok`, exit `0`. Two rendered frames (stopping,
  stopped) were inspected - the Cancel/Stopping button, the stopped banner and
  the unmeasured row all drew correctly - and were deleted with the harness.
- `build.bat` and `--selftest` after the cancel control: exit `0`, 35 checks,
  `RESULT: 35 passed, 0 skipped, 0 failed.`, including `[PASS] a cancelled scan
  leaves its row unmeasured -> scanned: no, bytes: 0 B, files: 0`.
- Per-row progress, driven by the real `StartScan` in a scratch console build
  with a hidden window: a pass over the eleven real targets reported
  `all_pending=1 progress_messages=11 monotonic=1 all_done=1 tick_kept=1`, and
  the crash-dump row that was unticked before the scan was still unticked
  after it. A second pass rendered the page once the quick rows had landed
  (`after_first=10`): the temporary-files row read `Measuring...` in the size
  column while the other ten showed their figures, and the status line read
  `Measuring folders... 10 of 11`. `RESULT: ok`, exit `0`; the harness and its
  scratch images were deleted.
- `build.bat` and `--selftest` after the progress change: exit `0`, 34 checks,
  `RESULT: 34 passed, 0 skipped, 0 failed.`
- The bulk controls were driven through the real handler by a scratch console
  build with a throwaway `LOCALAPPDATA`: with every row cleared first,
  **Select all** ticked 9 of the 11 rows - exactly the present, non-personal
  ones - wrote `user_temp=1`, left no `downloads` key, and a fresh launch read
  the same ticks; **Clear all** turned every row off, wrote `user_temp=0`, and
  a launch read it back off; **Reset to defaults** then returned the stock
  selection. `RESULT: ok`, exit `0`; the scratch settings folder and harness
  were deleted.
- Bulk presses and a tick made mid-scan, driven through the real handler by a
  scratch console build: after **Clear all** (`all_off=1`) a scan was started
  and the crash-dump row ticked while it ran; when the scan landed the list
  read `dumps_on=1 temp_off=1 measured=1` with every other row still off and
  the figures measured (temp 3.4 GB, Downloads 357 MB), so the measurement and
  the user's ticks coexist. `RESULT: ok`, exit `0`; the scratch settings folder
  and harness were deleted.
- `build.bat` and `--selftest` after the bulk controls: exit `0`, 34 checks,
  `RESULT: 34 passed, 0 skipped, 0 failed.`, including `[PASS] bulk ticks cover
  present targets and never Downloads -> present ticked: yes, missing left
  off: yes, Downloads left off: yes, clear all: yes`.
- `--shot` rendered the Cleanup page with the two links in the action row and
  the selection line still clear on the right.
- The reset control was driven through the real handlers by a scratch console
  build pointed at a throwaway `LOCALAPPDATA`: after clicking the temp cache
  off and Downloads on (which wrote `user_temp=0` and no key for Downloads),
  clicking `HitCleanupReset` left `cache_on=1 downloads_off=1 figure_kept=1
  section_gone=1 relaunch_defaults=1` - the section was removed from the file,
  a measured figure survived the reset, and the next launch read the stock
  selection. `RESULT: ok`, exit `0`; the scratch settings folder and the
  harness were deleted.
- `build.bat` and `--selftest` after the reset control: exit `0`, 33 checks,
  `RESULT: 33 passed, 0 skipped, 0 failed.`, including `[PASS] cleanup ticks
  reset to the built-in defaults -> caches ticked again: yes, stored keys
  left: none`.
- `--shot` rendered the Cleanup page with the new link at the bottom right,
  clear of the footer text and of the list's last row.
- Tick persistence, driven through the real row handler (`HandleHit`) by a
  scratch console build pointed at a throwaway `LOCALAPPDATA`: the defaults
  read `user_temp=1 crash_dumps=1 downloads=0`; clicking the temp cache wrote
  `user_temp=0`, clicking Downloads wrote no key for it at all (`<unset>`),
  and the next launch restored `user_temp=0 crash_dumps=1 downloads=0`.
  Re-ticking wrote `1` and a further launch read `1`. `RESULT: ok`, exit `0`;
  the scratch settings folder and the harness were deleted, so the real
  `settings.ini` was never touched.
- `build.bat` and `--selftest` after the persistence change: exit `0`, 32
  checks, `RESULT: 32 passed, 0 skipped, 0 failed.`, including `[PASS] cleanup
  ticks are remembered, Downloads never is -> opt-out restored: yes, untouched
  row kept its default: yes, Downloads armed after being saved: no`.
- The Downloads confirmation was driven for real by a scratch console build of
  the same sources with a hidden window, answering the actual dialog: the text
  read back from the dialog's own controls was `Your Downloads folder is
  ticked: 12.0 MB in 2 files will be moved to the Recycle Bin, where you can
  put them back.` / `Clean them now?`. **No** left the page idle with both
  files still on disk; **Yes** started the clean and left `Moved 12.0 MB to
  the Recycle Bin from 2 files.` with the files gone; a caches-only selection
  cleaned its file with no dialog at all. `RESULT: ok`, exit `0`; the harness
  and its binary were deleted, and the two files it recycled were restored out
  of the bin and removed.
- `build.bat` and `--selftest` after the confirmation: exit `0`, 31 checks,
  `RESULT: 31 passed, 0 skipped, 0 failed.`, including `[PASS] personal clean
  asks first, caches do not -> prompt names target, size and Recycle Bin: yes,
  cache prompt: none`.
- First-open measurement, driven through the real `NavigateTo` and `WndProc`
  by a scratch console build of the same sources with a hidden window:
  `first open: scanning=1 worker=1` immediately after navigating to Cleanup,
  then `measured: hasScanned=1 scanning=0 rows=11 present=10 scanned=11` with
  every row measured (temp 3.4 GB / 36,808 files, Downloads 357 MB / 11
  files, Windows Update downloads 37.2 MB / 5 files). Reopening the measured
  page logged `scanning=0 worker=0`, so it did not scan twice. Opening it
  while the worker was held by another job logged `scanning=0 worker=1`, and
  the 1 s tick after the worker was freed logged `scanning=1 worker=1` and
  finished. `RESULT: ok`, exit `0`; the harness and its binary were deleted.
- `build.bat` and `--selftest` after the open-measure change: exit `0`, 30
  checks, `RESULT: 30 passed, 0 skipped, 0 failed.`
- `build.bat` and `--selftest` after the recycle change: exit `0`, 30 checks,
  `RESULT: 30 passed, 0 skipped, 0 failed.`, the new one being "Downloads
  target recycles while caches still delete -> Downloads row recycled 2 files
  / 10 B, cache row deleted 3 B, folder kept".
- The recycled test items were checked in the real Recycle Bin, not just
  modelled: `Shell.Application` listed `a` and `folder` with `DeletedFrom`
  `C:\Users\Bored\AppData\Local\Temp\ZanpharsWinSuite-recycle`, so both the
  file and the whole folder were recoverable rather than unlinked.
- The batched shell calls were pushed past a single batch by a scratch
  console build linked against `src\core.cpp`: 150 loose files plus a folder
  of 5 more (155 items, three batches) reported `recycled=16000 bytes (0
  freed), files=155, skipped=0`, `left_on_disk=0 root_kept=1`, `RESULT: ok`.
- `--shot` rendered the Cleanup page with the new footer: "Only files inside
  the folders you tick are removed. Downloads is cleared only when you tick
  it, and those files go to the Recycle Bin until you empty it."
- Every scratch item the tests put in the Recycle Bin (192 of them, including
  earlier runs) was restored and deleted, so the bin and `%TEMP%` were left as
  they were found.
- `build_all.bat /clean`: cleared `bin\`, rebuilt the four programs, then ran
  the launcher stage - `Built bin\ZanpharsWinSuite.exe`, `Running self-test
  ...`, `[ OK ]` - and reported five self-contained executables, exiting `0`
  with `SUCCESS: 5 executable(s)`.
- Launcher failure path: a scratch copy of `build_all.bat` whose launcher call
  pointed at a stub that exits `1` reported
  `[FAIL] the suite launcher did not build or failed its self-test`, ended
  with `FAILED: 1 project(s) did not build` and exited `1`.
- `release.bat /build`: exit `0`, `SUCCESS: release\Zanphar-WinSuite-v0.2.0.zip`,
  and the archive holds ten entries including `ZanpharsWinSuite.exe`.
  `sha256sum -c SHA256SUMS.txt` reports `OK` for every file including the
  launcher, and the archive's own `.sha256` verifies.
- Integration build: `build.bat` exit `0` with no compiler diagnostics,
  `bin\ZanpharsWinSuite.exe` 1,983,774 bytes, self-test run from `bin\`.
- `--selftest`: 27 checks, 0 failures, including "7 catalogued, 2 installed"
  browsers, "22.8 KB in 2 file(s) for Google Chrome", "10 adapter(s), first:
  Bluetooth Network Connection", "power plan readable -> High performance"
  and the memory-options round-trip.
- `--shot _tmp\shots` rendered all ten pages at 1280 x 880 and every one
  wrote its bitmap. The new pages were inspected: Browsers listed Edge at
  683 MB across 5,660 files with the uninstalled browsers greyed out, DNS
  listed ten adapters each showing `8.8.8.8,8.8.4.4`, Boost showed the
  journalled plan and the three tweaks, and Memory showed the
  automatic-clean chips, the idle and clipboard toggles.
- `--selftest` after the Game DVR fix: 28 checks, 0 failures, the new one
  being "boost Game DVR values round-trip as REG_DWORD".
- `--selftest` after the skip and summary changes, on this PC: `build.bat`
  exit `0`, 28 `[PASS]`, no `[SKIP]`, `RESULT: 28 passed, 0 skipped, 0
  failed.`, exit `0` - the summary matches the `[PASS]` lines above it.
- The skip path, run against a scratch build of the same sources whose
  `LoadDnsAdapters` and `LoadServices` return nothing (a machine with no
  configured adapter and no third-party startup service): exit `0`,
  `[SKIP] third-party auto-start services enumerated -> this machine starts no
  third-party service with Windows`, `[SKIP] DNS adapters enumerated -> this
  machine has no configured adapter`, and `RESULT: 26 passed, 2 skipped, 0
  failed.` - 26 plus 2 is the full set, so skips are counted, not lost.
- Skips do not mask a failure: the same scratch build relinked without its
  version resource still reports `[FAIL] exe carries a version resource`,
  `RESULT: 25 passed, 2 skipped, 1 failed.` and exits `1`.
- New Cleanup targets on this PC, as listed by `--selftest`: Your Downloads
  folder (`C:\Users\Bored\Downloads`, 357 MB in 11 files, unticked), Windows
  Update downloads (`C:\WINDOWS\SoftwareDistribution\Download`, 37.2 MB in 5
  files), Windows Prefetch, live kernel reports, Windows minidumps and system
  error reports; 11 targets in all. `RESULT: 29 passed, 0 skipped, 0 failed.`,
  including the new "Downloads is listed and unticked by default -> present,
  off by default".
- `--shot` rendered the Cleanup page with all 11 rows: Downloads sat unticked
  with its `your files` pill while the temp caches stayed ticked, and the
  header and footer carried the new wording.
- Post-clean flow, exercised through the real handlers by a scratch build
  whose only cleanup group pointed at a throwaway folder (four 1 KB files).
  The log read `scanned: bytes=4096`, then the clean's completion, then
  `quiet scan called: page=Cleanup scanning=0 cleaning=0 hasScanned=1
  worker=0`, then `clean done: cleaning=0 scanning=0 hasScanned=1 bytes=0`:
  the folder was emptied and the page was left idle with the Scan button
  enabled, while the background refresh ran. The scratch build and folder
  were deleted afterwards.
- Post-clean item refresh, driven through the real handlers (`StartClean`,
  `FinishClean`, `FinishCleanRefresh`) by a scratch console build of the same
  sources: a two-row list, one armed row pointing at a throwaway folder of two
  1 MB files, cleaned to empty. `FinishClean` left `cleaning=0 scanning=0`
  with the `Freed ...` banner set, and the refresh then replaced the two
  synthetic rows with all 11 real targets. Every refreshed row matched a fresh
  measurement byte-for-byte and file-for-file, the cleaned row stayed
  unticked, and a tick made while the refresh was in flight survived it.
  `RESULT: 0 failure(s).` The harness, its folders and the scratch binary were
  deleted afterwards.
- `build.bat` and `--selftest` after the refresh change: exit `0`, 29 checks,
  `RESULT: 29 passed, 0 skipped, 0 failed.`
- Live apply and restore of the Boost and DNS engines on this PC, run
  elevated through a throwaway harness linked against `src\boost.cpp` and
  `src\dns.cpp` (its scratch file was deleted afterwards). Because this PC
  keeps every parkable service on manual start, has Game DVR off and already
  sits on High performance, the harness put DPS on automatic, switched Game
  DVR on and moved the plan to Balanced first, then undid all three at the
  end. `ApplyBoost` reported `Boost on: 3 change(s) applied`: the plan became
  High performance, DPS went automatic/running to manual/stopped, Game DVR
  went `1` to `0`. `RestoreBoost` reported `Restored: 3 change(s) put back`,
  returned the plan to the Balanced plan that was live when the boost was
  applied, restarted DPS on automatic and put Game DVR back to `1`, removed
  every journal entry and left `Active = 0`. `ApplyDns(Google)` and
  `ApplyDns(Cloudflare)` rewrote all ten adapters, the watchdog reverted a
  tampered adapter in a single pass, `RestoreSystemDns` cleared all ten
  overrides, and the machine's own values (mostly `8.8.8.8,8.8.4.4`, one
  `192.168.137.1,8.8.4.4`) were written back exactly. The run printed 30
  `[PASS]` lines and `RESULT: all live checks passed.`
- That live run's leftovers were then read back outside the harness: every
  adapter's `NameServer` matches its pre-run value, the active scheme is High
  performance again, `GameDVR_Enabled` is `0`, the boost journal holds only
  `Active = 0` with no per-service entries, and all seven parkable services
  are on manual start as they were found (DPS running, WMPNetworkSvc
  stopped).
- Rename rebuild with `build_all.bat`: exit `0`, five outputs, and
  `tools\verify_binaries.ps1` reports 5/5 executables self-contained (98 DLL
  imports, all Windows-provided). Each exe's `OriginalFilename` now matches its
  file name, and the embedded manifests read `name="DNSProtect"` and
  `name="SystemTurbo"`.
- The inlined fallback was exercised for real: a throwaway `ProbeTool\` folder
  with a single `src\probe.cpp` was picked up, compiled with the generic
  recipe, copied to `bin\ProbeTool.exe`, flagged under "built with the generic
  recipe" and verified; the folder and binary were then deleted.
- `build.bat` after the move: exit `0`, `bin\ZanpharsWinSuite.exe` relinked
  (1,759,085 bytes), self-test run from `bin\` reports 22 `[PASS]`, 0
  `[FAIL]`, "all checks passed". `tools\check-icon.ps1` finds 1 icon
  resource with 9 frames in that exe and exits `0`.
- `tools\verify_binaries.ps1` on the current `bin\`: 4/4 executables, 79 DLL
  imports, all Windows-provided, exit code `0`.
- Rebuilt with `release.bat /build` after adding Smart RAM's status line: exit
  `0`, `SmartRAM.exe` at 397 KB, self-contained check 4/4 executables, 79
  DLLs. The running build was then checked visually by posting the
  clean-finished message (`WM_APP + 3`) with 123456789, 41943040 and 0 bytes
  — the same message the cleaner thread sends, so no working set was
  trimmed: the card drew `117.74 MB FREED` in green, `40.00 MB FREED` in
  grey and `NOTHING TO FREE`, with the card measuring 316×436 and the line
  sitting under `CLEAN NOW`.
- Rebuilt after removing Smart RAM's notifications with `release.bat /build`:
  exit `0`, all four binaries rebuilt, `SmartRAM.exe` down from 400 KB to
  395 KB, and none of the notification texts (`Memory released`, `fully
  optimized`, `Widget hidden`) appear in the new executable. The self-
  contained check still reports 4/4 executables, 79 DLLs, all Windows-
  provided.
- Negative cases: an older single-project build that imports
  `libwinpthread-1.dll` is reported as `not self-contained` with exit code
  `1`; a missing `-Expected` output and an empty folder also exit `1`.
- The workflow's toolchain-locating step was run for both outcomes: with a
  complete `ucrt64\bin` it appends a UTF-8 (no BOM) `MINGW_BIN` line to
  `$GITHUB_ENV`; with `windres.exe` missing it aborts with exit code `1` and
  writes nothing.
- `release.bat /build` end to end: exit code `0`; all four executables in
  `bin\` were produced during the run, the packaged files are byte-identical
  to `bin\`, and both `sha256sum -c` checks still pass.
- The CI build step's own body (`build.yml` step 5, extracted verbatim and run
  through `cmd.exe`) was executed on this machine: exit `0`, `bin\` rebuilt
  into all four executables with new timestamps, and
  `release\Zanphar-WinSuite-v0.2.0.zip` plus `.zip.sha256` written. The two
  artifact globs (`bin/*.exe`, `release/*.zip` + `release/*.zip.sha256`)
  resolve to exactly those six files.
- The `/build` guard: with a build that exits `1`, the run stops after the
  build, writes no archive, creates no `release\` folder and exits `1`; a
  skipped or stale binary in `bin\` is never packaged, because the clean
  build removes the folder first.
- Argument handling: `/build` combines with `/version` in either order, and
  a bad version is still rejected before any compilation starts. `/help`,
  unknown arguments and a valueless `/version` behave as documented, and
  plain `release.bat` keeps its `[1/2]` packaging-only flow.
- Each `run:` body of the release job was extracted from `build.yml` and
  executed locally: the tag check accepts `v0.2.0` (writing
  `SUITE_VERSION=0.2.0` to a UTF-8/no-BOM `$GITHUB_ENV`) from a directory that
  holds only `VERSION` — it no longer needs `bin\` — and rejects `v0.2.1` (tag
  disagrees with `VERSION`), `vfoo` and `v` (no usable version), all with exit
  code `1` and no environment written. The checksum step re-hashes the
  uploaded archive and matches it to its `.sha256`; with the archive missing
  it exits `1` naming the file the build job should have uploaded.
- The release-notes step, run as the extracted step: for `0.2.0` it writes a
  61-line `release-notes.md` that is byte-identical to the section between
  `## [0.2.0] - 2026-10-22` and the next `##` heading, in UTF-8 without a BOM
  and with no CR, subsections and em dashes intact. A missing section, a
  section that holds only whitespace, and a heading at end of file all exit
  `1`; a CRLF changelog produces an LF body. The step pins `-Encoding UTF8`
  because Windows PowerShell 5.1's default ANSI read turned every em dash
  into mojibake.
- The publish step was run against a stub `gh` on `PATH`: a missing release
  produces `release create v0.2.0 <zip> <zip.sha256> --verify-tag
  --notes-file release-notes.md --title "Zanphar's WinSuite v0.2.0"`, an
  existing release produces `release upload v0.2.0 <files> --clobber`
  followed by `release edit v0.2.0 --notes-file release-notes.md`, a
  `v0.3.0-rc1` version adds `--prerelease`, and a failing `create`, `upload`
  or `edit` exits `1`.
- `build.yml` parses as YAML: two jobs, the build job runs `release.bat
  /build` and uploads two artifacts, the release job is gated on it with
  `contents: write` and the expected steps and action versions
  (`checkout@v7`, `upload-artifact@v7`, `download-artifact@v8`). It also
  parses with a `workflow_dispatch.inputs.tag` string input, the tag
  checkout (`github.event.inputs.tag || github.ref`) on both jobs, and the
  publish gate `github.ref_type == 'tag' || (github.event_name ==
  'workflow_dispatch' && github.event.inputs.tag)`.
- The release job's steps were re-run with `TAG` set the way a dispatch sets
  it: `v0.2.0` yields `SUITE_VERSION=0.2.0`, a 61-line `release-notes.md` and
  a matching archive checksum; an empty `TAG` exits `1` with "no tag to
  publish"; `nightly-1` exits `1` as not a usable version. With the stub
  `gh`, dispatch-style publishing issues `release create … --verify-tag
  --notes-file release-notes.md` when no release exists, and `release upload
  … --clobber` plus `release edit … --notes-file` when one does.
- The changelog gate, run as the extracted step: `0.2.0` matches
  `## [0.2.0] - 2026-10-22` and passes; `0.3.0` (only `[Unreleased]` present)
  exits `1`; a `###` heading, a mention in body text only, and `[0.4]` when
  the heading is `[0.4.0]` all exit `1`; headings without a date, an `-rc1`
  version and a CRLF file all pass.

### Known issues

- Cleaning a browser's traces from the Browsers page is wired and compiled but
  has not been run on this machine, because it deletes files. Its read paths
  (catalog, measurement) are covered by `--selftest`; the DNS and Boost pages
  have now been through a live apply and restore (see Verified).
- The Browsers page reads the `Default` profile of each Chromium browser, the
  `Opera Stable`/`Opera GX Stable` profiles and every Firefox profile. Extra
  Chromium profiles ("Profile 1" and so on) are not listed yet.
- DNS Protect's custom server pair has no equivalent in the suite: the DNS
  page offers Automatic plus the four public providers, and a custom pair
  still needs the standalone program.
- Smart RAM's status line was checked by posting the clean-finished message
  to the running window, which skips `CleanAll`: the real clean path (standby
  purge, working-set trim) has not been run on this machine, and the number
  was never produced by an actual clean. Click `CLEAN NOW` once to see it end
  to end.
- The workflow has not run on GitHub yet: this folder is not a git repository,
  so the runner-only parts (MSYS2 install, build on Windows Server 2025, the
  release job's artifact download and `gh release create`) are verified by
  construction and by their local equivalents, not by a real run. No tag has
  been pushed and no release exists.
- `msystem: UCRT64` was chosen because `setup-msys2` deprecates `MINGW64` and
  warns about it; `build_all.bat`'s own toolchain search still looks at
  `mingw64` folders, so on the runner the path must come from `MINGW_BIN`.

## [0.2.0] - 2026-10-22

Release packaging: `bin\` can now be turned into a versioned, verifiable
archive in one step.

### Added

- `release.bat` — release step and entry point.
  - Reads the suite version from `VERSION`, or `/version X.Y.Z` on the
    command line, and validates it.
  - Packages every `*.exe` in `bin\` together with `README.md`,
    `CHANGES.md` and `LICENSE.md` into
    `release\Zanphar-WinSuite-v<version>\`, zips it to
    `release\Zanphar-WinSuite-v<version>.zip`, and writes
    `<archive>.zip.sha256` over the archive.
  - `/help`, exit code `0` on success and `1` on every failure path.
- `tools\make_release.ps1` — the worker behind `release.bat`: staging,
  SHA-256 hashing, `MANIFEST.txt`, `SHA256SUMS.txt` and the ZIP.
- `MANIFEST.txt` in every package — per program the product name, file name,
  file/product version, exact size, description, publisher and full
  SHA-256, all read from the executable's own version resource, plus a
  `VERIFY` block with the commands to check the package.
- `SHA256SUMS.txt` in every package, `sha256sum -c` compatible.
- `VERSION` — single source of truth for the suite version.
- `release\` — release output, per version and independent of `bin\`.

### Fixed

- Checksum files were first written with CRLF line endings; `sha256sum -c`
  treats the trailing `\r` as part of the file name and fails every entry.
  `SHA256SUMS.txt` and `*.zip.sha256` are now LF-only.
- ZIP entries were first created through `Compress-Archive` (and then
  `ZipFile::CreateFromDirectory`), which on Windows PowerShell 5.1 store
  backslashes in entry names. Extractors on macOS and Linux then produce one
  file literally named `folder\program.exe`. Entries are now written by hand
  with forward slashes.
- A failed packaging run no longer leaves a truncated or zero-byte archive
  next to the good ones.

### Verified

- `release.bat` on the real tree: exit code `0`, four programs in the
  manifest, package and archive created under `release\`.
- Extracted the archive and ran `sha256sum -c SHA256SUMS.txt`: all eight
  packaged files `OK`; `sha256sum -c` on the archive's own checksum: `OK`.
- Every hash in `MANIFEST.txt` matches both `SHA256SUMS.txt` and the file on
  disk, and every size matches the staged executable.
- Archive entries use `/` separators and the versioned folder as the single
  top-level directory.
- Failure paths return `1`: empty or missing `bin\`, an invalid version
  token, an unknown argument. `/help` returns `0`.
- Packaging a second version leaves the first version's folder, archive and
  checksum untouched.

### Known issues

- Releases are not byte-for-byte reproducible: the archive stores file
  timestamps, so rebuilding the same sources yields a different ZIP digest.
  Rebuild and re-release rather than comparing archive hashes.
- The release step needs `powershell.exe` (Windows PowerShell 5.1 is enough);
  it is not available in a bare Windows PE and there is no cmd-only fallback.
- `release\` grows by one folder and two files per version; old versions are
  kept deliberately and must be deleted by hand.

## [0.1.0] - 2026-10-22

First packaged release of the suite: all four programs build from a single
script into `bin\`.

### Added

- `build_all.bat` — suite build script.
  - Toolchain discovery (`MINGW_BIN`, `PATH`, WinGet/WinLibs, common
    install locations) with a clear error and `winget` hint when MinGW is
    missing.
  - Space-free staging area in `%TEMP%` so no intermediates are written into
    the project folders.
  - Automatic per-project discovery of `src\*.cpp`, the resource script and
    the application manifest; one static link line per known project via the
    `PROJECTS` recipe table.
  - Fallback build for new folders that are not yet in the recipe table,
    flagged as "generic recipe" in the summary.
  - Import-table verification with `objdump`; the run fails if an output
    depends on `libgcc`, `libstdc++` or `libwinpthread`.
  - `/clean` (wipe `bin\` and staging) and `/help`.
  - Exit code `0` on full success, `1` otherwise.
- `bin\` with static, self-contained release builds of all four programs.
- `README.md` — full inventory of the folder and sub-folders, build
  instructions, per-program reference, and the contract for adding future
  programs.
- `CHANGES.md`, `LICENSE.md`.

### Fixed

- `windres` failed on resource scripts in paths containing an apostrophe
  (`Zanphar's WinSuite`). Resources are now copied into the staging folder
  and compiled there; no project source changed.
- The MinGW manifest override (`-B` search prefix) failed when the project
  path contained a space, which is what made
  `DNSProtect\build_mingw.bat` and
  `SystemTurbo\build_mingw.bat` unusable in this location. The suite
  script stages to a space-free path instead.
- Smart RAM had no manifest path under MinGW. Its standalone
  `res\app.manifest` is now embedded through a generated override object, so
  the binary keeps `PerMonitorV2` DPI awareness instead of falling back to
  the toolchain's bare `asInvoker` manifest.
- Smart RAM includes the MSVC-SDK-only header `shlobj_core.h`; a generated
  compatibility header maps it to `shlobj.h` + `knownfolders.h` during the
  suite build.
- An `echo` line containing `->` was interpreted as output redirection and
  dropped stray files next to the script.

### Verified

- Clean rebuild of all four projects: 4/4 outputs, exit code `0`.
- Failure path: a deliberately broken project reports `[FAIL]` and makes the
  script exit with `1`.
- Every output imports only Windows-provided DLLs; no MinGW runtime DLL is
  needed.
- Exactly one manifest per binary, with the intended privilege level
  (`requireAdministrator` for DNS Protect and System Turbo, `asInvoker` for
  Browser Cleaner and Smart RAM).
- `BrowserCleaner.exe` and `SmartRAM.exe` launch from `bin\` and open their
  windows. DNS Protect and System Turbo were not launched because their
  manifests raise a UAC prompt.

### Known issues

- `build_mingw.bat` in `DNSProtect\` and `SystemTurbo\` still
  fails inside `Zanphar's WinSuite\` because of the space in the folder
  path. Use `build_all.bat`, or build from a space-free checkout.
- `BrowserCleaner\BrowserCleaner.exe`,
  `SystemTurbo\SystemTurbo.exe` and `SmartRAM\build\`
  contain artifacts from earlier single-project builds; they are not
  produced or used by the suite script.

## Program releases

Each program started as its own project before the suite existed. Their
initial releases are dated by the day the program landed in this folder.

### Browser Cleaner 1.0.0.0 — 2026-10-20

- Initial release. Removes history, cookies, autofill forms, cached files and
  session restore data from Chrome, Edge, Brave, Vivaldi, Opera, Opera GX and
  Firefox, including SQLite sidecar files.
- Detects and offers to close running browsers (Toolhelp32); measures and
  reports what was cleaned per browser and per trace item.
- GDI+ dark UI, per-monitor-v2 DPI aware, single source file, no elevation,
  no configuration file.

### DNS Protect 1.0.0.0 — 2026-10-20

- Initial release. Enforces a trusted DNS configuration on every IP-enabled
  adapter (current settings, Google, Cloudflare, OpenDNS, Quad9, Comodo,
  Verizon, DNS Advantage, or a custom pair).
- Watchdog reverts unauthorized `NameServer` / `DhcpNameServer` changes
  within ~1.5 s and logs each incident.
- Dark dashboard with live adapter list and tray integration; manifest-driven
  elevation with a read-only fallback when UAC is declined.
- State in `%ProgramData%\DNSProtect\` (`dnsprotect.ini`,
  `dnsprotect.log`).

### Smart RAM 1.0.0.0 — 2026-10-20

- Initial release. Floating Direct2D monitor card with animated RAM gauge,
  used/total, free memory, CPU load and process count at 2 Hz.
- One-click clean: standby-list purge plus working-set trimming, with a
  freed-memory toast and tray balloon.
- Animated tray icon, settings card, Smart Clean threshold (50–95 %), Clean
  When Idle, Clear Clipboard, Always on Top, Start with Windows.
- Per-pixel-alpha layered windows, per-monitor-v2 DPI aware, remembered
  drag position with per-monitor bounds checking, single instance.
- Settings in `%LOCALAPPDATA%\SmartRAM\config.ini`.

### System Turbo 1.0.0.0 — 2026-10-20

- Initial release. Reversible boost: High Performance power plan, service
  start types parked in manual mode, Game DVR recording disabled — each
  change captured in restore records that survive a restart.
- Live RAM/CPU card with the heaviest images per executable; memory release
  on demand, on boost, and automatically at 80 % RAM load.
- Dark dashboard, tray integration, manifest-driven elevation with an
  instrumentation-only fallback when UAC is declined.
- State in `%ProgramData%\SystemTurbo\` (`systemturbo.ini`,
  `systemturbo.log`).
