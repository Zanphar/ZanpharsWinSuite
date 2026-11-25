# Home Policy

A Group Policy Editor for Windows 10 Home and Windows 11 Home: the policy
switches the official editor would give you, in a window a beginner can read.

Windows Home ships without `gpedit.msc`. The components that *read* policies are
present, though - Explorer, Windows Update, Defender, Edge and the rest check the
same `HKLM\SOFTWARE\Policies` values on Home that they check on Pro - and those
values can be written by any program with administrator rights. Home Policy is
that program: thirty-seven settings, each one a named registry value, with a
plain sentence saying what it does and three choices on the row.

```
HomePolicy.exe                                    open the window
HomePolicy.exe --apply "Widgets board" disabled   write one setting and read it back
HomePolicy.exe --list                             print every setting and what this PC reads
HomePolicy.exe --selftest                         prove the catalog and the registry engine
HomePolicy.exe --help                             the same list on the console
```

## Using it

1. Pick a category on the left, or type in **Search settings** to filter by name,
   description or registry path.
2. On a card, choose **Enabled**, **Disabled** or **Not configured**. Nothing is
   written yet: the card gets a blue outline and a dot, and the footer counts the
   staged changes.
3. Press **Apply changes**. Windows asks for administrator rights once; the
   window reopens elevated **carrying the staged rows on its command line**, so
   nothing you chose is lost in the handover. That window writes exactly those
   rows, reads the registry back, and reports what landed. Declining the prompt
   leaves the original window open with every choice still staged.
4. **Discard** puts every staged row back. **Back up...** writes the current
   policy values to a `.reg` file you can double-click to restore.

The footer carries the copyright - **© 2026-2027 Charles McDonald** - bottom
left, under a hairline rule, with **Restart as administrator** on the same row
when the window is not elevated.

**Not configured** means "remove the value and let Windows decide". That is the
state a fresh install is in, and it is the way back from every row here. Applied
rows say when they take effect - `Immediately`, `Sign out`, `Restart` or
`Restart Explorer` - under the three buttons.

The window opens as a normal user: reading policies needs no rights, so the whole
catalog is visible before any prompt. Only Apply asks for elevation, and declining
the UAC prompt leaves everything readable and nothing changed.

## One setting from a shell

`--apply` writes a single row, reads the registry back and prints what it found,
so a support session or a script can make one change and prove it took:

```
> HomePolicy.exe --apply "Widgets board" disabled

Home Policy - apply one setting

  setting : Turn off the Widgets board
  path    : HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Dsh  -  AllowNewsAndInterests
  before  : Not configured
  asked   : Disabled
  reads   : Disabled  (value 0x00000000)
  applies : Restart Explorer
  result  : OK
```

- **The title may be any unique part of one.** Matching is case-insensitive;
  an exact title always wins, and a phrase that fits several rows lists them and
  exits 2 so nothing is written by accident.
- **States** are `enabled`, `disabled` and `not-configured` (also `on`, `off`,
  `reset`, `default`).
- **Exit codes**: 0 the row reads what was asked for, 1 the write or read-back
  failed, 2 the arguments were wrong or ambiguous. A failed write says whether
  elevation was the reason; if Windows itself refused the key, it says that
  instead - some builds protect a policy key against every writer, `reg.exe`
  included.
- Some keys need an elevated shell: every machine policy, and the per-user
  `CurrentVersion\Policies` keys Windows locks to administrators. The mode
  changes nothing else: no staging, no window, no other row.

## What it can set

Thirty-seven settings in seven categories. Every one is a policy value - the same
value the official editor writes - not a general-purpose registry tweak. Every
row was checked against the ADMX files in `C:\Windows\PolicyDefinitions` and
against Microsoft's policy documentation; the paths below are the ones those
files define, including the hive each policy is declared for. The one row that
build carries no ADMX for is **Turn off Windows Script Host**, which writes the
registry setting the host itself reads rather than an administrator-template
value; the table says so on the row.

### Windows Update

| Setting | Value | Effect |
|---|---|---|
| Never restart while I am signed in | `WindowsUpdate\AU\NoAutoRebootWithLoggedOnUsers = 1` | Immediately (only with scheduled updates) |
| Keep driver updates out of Windows Update | `WindowsUpdate\ExcludeWUDriversInQualityUpdate = 1` | Restart - not documented for Home |
| Hold back a new Windows version for 180 days | `WindowsUpdate\DeferFeatureUpdates = 1`, `DeferFeatureUpdatesPeriodInDays = 180` | Restart |
| Hold back monthly quality updates by 7 days | `WindowsUpdate\DeferQualityUpdates = 1`, `DeferQualityUpdatesPeriodInDays = 7` | Restart |

Both delay rows are marked **Home ignores this**: update deferral is a Windows
Update for Business feature Microsoft documents for Pro, Enterprise and
Education, so the values are written and this PC will not act on them.

The driver-exclusion row carries **Not documented for Home** for the same reason -
Microsoft scopes that policy to Pro, Enterprise and Education - but it is not
tagged as ignored: the value is read by the same Windows Update client on every
edition, so it may well take effect. The card says what Microsoft documents, not
more.

Microsoft also scopes *Never restart while I am signed in* to devices that
install updates on a schedule, and its own guidance warns the policy "doesn't
work exactly as per description"; the row now says so.

### Privacy & telemetry

| Setting | Value | Effect |
|---|---|---|
| Turn off the advertising ID | `AdvertisingInfo\DisabledByGroupPolicy = 1` | Sign out |
| Stop Windows asking me for feedback | `DataCollection\DoNotShowFeedbackNotifications = 1` | Immediately |
| Turn off my activity history | `System\PublishUserActivities = 0`, `UploadUserActivities = 0` | Immediately |
| Turn off clipboard sync between devices | `System\AllowCrossDeviceClipboard = 0` | Sign out |
| Turn off location for every app | `LocationAndSensors\DisableLocation = 1` | Sign out |
| Turn off Windows Error Reporting | `Windows Error Reporting\Disabled = 1` | Restart |
| Turn off tips written from my diagnostic data | `HKCU\SOFTWARE\Policies\Microsoft\Windows\CloudContent\DisableTailoredExperiencesWithDiagnosticData = 1` | Sign out |
| Turn off suggested apps and promoted content | `CloudContent\DisableWindowsConsumerFeatures = 1` | Sign out |
| Turn off Windows tips and welcome screens | `CloudContent\DisableSoftLanding = 1` | Sign out |
| Turn off speech and handwriting personalisation | `InputPersonalization\AllowInputPersonalization = 0` | Sign out |

There is no telemetry-level row. Microsoft documents `AllowTelemetry = 0` as
equal to `1` on Home, so a row writing 0 would change nothing; it was dropped
rather than left on the card claiming otherwise.

### Start, search & taskbar

| Setting | Value | Effect |
|---|---|---|
| Turn off web results in Start search | `Windows Search\DisableWebSearch = 1`, `ConnectedSearchUseWeb = 0` | Restart Explorer |
| Turn off Cortana | `Windows Search\AllowCortana = 0` | Restart |
| Turn off Search highlights | `Windows Search\EnableDynamicContentInWSB = 0` | Immediately |
| Turn off the Widgets board | `Dsh\AllowNewsAndInterests = 0` | Restart Explorer |
| Turn off Windows Copilot | `WindowsCopilot\TurnOffWindowsCopilot = 1` | Restart Explorer |

Cortana and Copilot carry the **Deprecated by Microsoft** tag: newer Windows 11
builds have removed Cortana entirely, and `TurnOffWindowsCopilot` is a user-scope
policy Microsoft has retired - it does not govern the current Copilot app. On the
card the tag sits on its own line under the path, so it never shortens the registry
path (a tagged card is one line taller than an untagged one).

### File Explorer

| Setting | Value | Effect |
|---|---|---|
| Stop keeping a history of recent documents | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoRecentDocsHistory = 1` | Sign out |
| Turn off thumbnail caching | `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoThumbnailCache = 1` | Restart Explorer |
| Turn off window shaking | `HKCU\SOFTWARE\Policies\Microsoft\Windows\Explorer\NoWindowMinimizingShortcuts = 1` | Restart Explorer |
| Turn off the Windows key shortcuts | `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoWinKeys = 1` | Sign out |
| Turn off Store suggestions in Open with | `HKLM\SOFTWARE\Policies\Microsoft\Windows\Explorer\NoUseStoreOpenWith = 1` | Restart |

Three of these moved to user scope because their ADMX policies are declared
`class="User"`: a machine-wide copy of `NoThumbnailCache` or `NoWinKeys` is
ignored, and `NoWindowMinimizingShortcuts` lives under
`SOFTWARE\Policies\Microsoft\Windows\Explorer`, not `CurrentVersion`.
`NoUseStoreOpenWith` exists for both scopes and uses the `Policies` path, not
`CurrentVersion`.

### System & apps

| Setting | Value | Effect |
|---|---|---|
| Turn off Game DVR and background recording | `GameDVR\AllowGameDVR = 0` | Sign out |
| Turn off OneDrive sync | `OneDrive\DisableFileSyncNGSC = 1` | Restart |
| Stop background apps for everyone | `AppPrivacy\LetAppsRunInBackground = 2` | Sign out |

### Security

| Setting | Value | Effect |
|---|---|---|
| Block potentially unwanted apps | `Windows Defender\PUAProtection = 1` | Immediately |
| Turn off LLMNR name lookup | `Windows NT\DNSClient\EnableMulticast = 0` | Restart |
| Record PowerShell script blocks in the event log | `PowerShell\ScriptBlockLogging\EnableScriptBlockLogging = 1` | Immediately |
| Turn off Windows Script Host | `HKLM\SOFTWARE\Microsoft\Windows Script Host\Settings\Enabled = 0` | Immediately |
| Turn off Remote Assistance invitations | `Windows NT\Terminal Services\fAllowToGetHelp = 0` | Immediately |
| Turn off AutoPlay for every drive | `CurrentVersion\Policies\Explorer\NoDriveTypeAutoRun = 0xFF` | Sign out |

### Microsoft Edge

| Setting | Value | Effect |
|---|---|---|
| Skip Edge's first-run tour | `Edge\HideFirstRunExperience = 1` | Restart Edge |
| Turn off the Edge sidebar | `Edge\HubsSidebarEnabled = 0` | Restart Edge |
| Turn off the Edge shopping assistant | `Edge\EdgeShoppingAssistantEnabled = 0` | Restart Edge |
| Turn off Edge feature recommendations | `Edge\ShowRecommendationsEnabled = 0` | Immediately |

The old `PromotionalTabsEnabled` policy is deprecated by Microsoft and
`ShowRecommendationsEnabled` is its replacement; the row writes the current one.

Short paths above are relative to `HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Windows`;
`Edge\` is `SOFTWARE\Policies\Microsoft\Edge` under `HKLM`, and `HKCU\` rows
live under `HKEY_CURRENT_USER` and apply to the signed-in account only. The
elevated copy writes those in the same user's hive, and `Back up...` reads them
from there. The window shows the full path on every card, and `--list` prints it
verbatim.

## Three states, and why Disabled writes a value

- **Not configured** removes the value. This is the Windows default.
- **Enabled** writes the policy's on-data.
- **Disabled** writes its off-data - usually `0`, and the opposite number for the
  policies whose "on" is `0`. It does *not* remove the value, so the three states
  stay distinguishable in the registry: absent, on, off. The official editor keeps
  that distinction in its own policy store, which Home does not have.

A value set to a third number by hand reads as **Custom**; the window leaves it
alone until you pick one of the three states, which replaces or removes it.

## Backup

**Back up...** writes a UTF-16 `.reg` file with a comment header and one key
block per policy that currently has a value. Opening the file puts those values
back - it is a record, not a restore point for unrelated settings, and it only
contains the values this catalog manages.

## Self-test

`--selftest` proves the parts that can be proven without changing a real Windows
setting:

- the catalog has no empty titles, no row outside a real category, no value
  written twice, and at least thirty entries;
- the three-state mapping classifies absent, on, off and third values correctly;
- a synthetic policy round-trips **enabled → disabled → not configured** through
  `HKCU\Software\ZanpharsWinSuite\HomePolicy\selftest`, which the test then
  deletes.

It prints PASS/FAIL per check and exits non-zero on a failure, so a build can gate
on it. `--list` reads the real catalog and prints every setting with its state and
full registry path; it changes nothing and needs no elevation.

## Limits, stated plainly

- **No ADMX files.** The catalog is the thirty-seven rows above, compiled in. A
  policy that is not in the list is not in the program.
- **Home still has the final word.** Microsoft documents nearly every policy
  here for Pro, Enterprise and Education, and none of them for Home. The values
  are the ones the owning components read, but where Microsoft states that Home
  behaves differently the card says so: the two update-deferral rows read **Home
  ignores this**, and the retired Cortana and Copilot policies read **Deprecated
  by Microsoft**. The tool writes the value; whether Windows honours it is
  Windows' decision.
- **No `gpupdate`.** Windows applies these values when the component that owns
  them next starts - that is what the `Applies:` line on each card is for. The
  tool never runs a command line.
- **One write path.** `RegCreateKeyEx` / `RegSetValueEx` / `RegDeleteValue` under
  the 64-bit view, nothing else. No service, no driver, no scheduled task.
- **Reversible per row.** Set a row back to **Not configured** and the value is
  gone; that is the same state the machine was in before the tool touched it.
- **A protected key can still refuse.** Windows 11 builds guard a few policy
  keys against every writer, `reg.exe` included; Home Policy reports the refusal
  and leaves the row as it was rather than claiming a change.

## Checking the catalog against the ADMX files

Every row names a registry value, and the authority for those names is the ADMX
file Windows ships. `tools\check-admx.ps1` reads `src\catalog.cpp`, finds each
value in `C:\Windows\PolicyDefinitions` and prints a drift report covering the
four things that can be wrong with a row:

| Checked | What it means |
|---|---|
| key | the path the ADMX declares, hive aside |
| value | the value name the ADMX declares |
| scope | the policy's class: `Machine` -> HKLM, `User` -> HKCU, `Both` -> either |
| data | the enabled/disabled numbers, or the allowed range or item list |

The catalog is effect-oriented - a row titled "Turn off X" writes the policy's
*disabled* data as its own on - so a pair that matches in the other order is
reported `ok ... inverted` rather than as drift. What is drift is a number the
ADMX never mentions, a key it does not name, or a value name it does not declare.

```bat
powershell -ExecutionPolicy Bypass -File HomePolicy\tools\check-admx.ps1
```

It prints one line per value, then `CLEAN` or `DRIFT` with a count, and exits 1
when anything drifted, so it can gate a change. It reads files only: nothing is
written to the registry and no network is used. `-CatalogPath` points it at an
edited copy of the catalog, `-AdmxPath` at another Windows version's templates.

Two kinds of row are expected to have no policy definition. The Windows Script
Host row writes a documented registry setting rather than an administrator
template, and the four Edge rows live in `MSEdge.admx`, which Windows does not
ship. Both are listed in the script with the reason; pass `-ExtraAdmxPath` with a
folder holding `MSEdge.admx` and the Edge rows are checked like any other. Any
other value with no ADMX is reported as `NO-ADMX` and fails the run.

## Icon

The suite's dark card with three slider tracks - the settings mark - and **GP**
over them. The sliders are a watermark at 32 px and up; at 16 and 24 px, where
the letters would be a smudge, the sliders are drawn alone, full width and
brighter. Sizes 16 / 24 / 32 / 48 / 64 are uncompressed 32-bit DIBs, 128 and 256
are PNG, all in `res\app.ico`, embedded as resource id 2 and set on the window
class so the title bar, taskbar button and Explorer all show the same tile.

Regenerate with:

```bat
powershell -ExecutionPolicy Bypass -File HomePolicy\tools\make_icon.ps1
```

## Building

```bat
HomePolicy\build_mingw.bat      this program only
build_all.bat                   the whole suite
```

Both link statically with `-municode -mwindows`; the only DLLs the output uses
are Windows' own (`advapi32`, `comdlg32`, `dwmapi`, `gdi32`, `kernel32`,
`shell32`, `user32` and the Universal CRT).
