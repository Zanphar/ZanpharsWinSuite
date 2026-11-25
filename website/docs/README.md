# Zanphar's WinSuite

Nine independent Windows utilities that grew out of one idea: rebuild the
everyday "PC tune-up" toolbox from scratch, in plain C++17 / Win32, with
**zero third-party dependencies** and a consistent dark, DPI-aware look.

Every program here is a from-scratch implementation inspired by a commercial
module. No code, resource or binary from the original products is used, and
none of them are affiliated with, endorsed by, or supported by the vendors
they take inspiration from.

| Program | Output | Version | Elevation | Data |
|---|---|---|---|---|
| [Browser Cleaner](#browser-cleaner) | `bin\BrowserCleaner.exe` | 1.0.0.0 | asInvoker | none (edits browser profiles) |
| [DNS Protect](#dns-protect) | `bin\DNSProtect.exe` | 1.0.0.0 | requireAdministrator | `%ProgramData%\DNSProtect\` |
| [Smart RAM](#smart-ram) | `bin\SmartRAM.exe` | 1.0.0.0 | asInvoker | `%LOCALAPPDATA%\SmartRAM\config.ini` |
| [System Turbo](#system-turbo) | `bin\SystemTurbo.exe` | 1.0.0.0 | requireAdministrator | `%ProgramData%\SystemTurbo\` |
| [HardWin](#hardwin) | `bin\HardWin.exe` | 1.0.0.0 | asInvoker (prompts when asked) | registry journal under `HKLM`/`HKCU` |
| [PCSpeedTest](#pcspeedtest) | `bin\PCSpeedTest.exe` | 1.0.0.0 | asInvoker | `%LOCALAPPDATA%\PCSpeedTest\results.csv` |
| [Home Policy](#home-policy) | `bin\HomePolicy.exe` | 1.0.0.0 | asInvoker (prompts on Apply) | none (policy values under `HKLM`/`HKCU`) |
| [Internet Tuning Wizard](#internet-tuning-wizard) | `bin\InternetTuningWizard.exe` | 1.0.0.0 | asInvoker (prompts on Apply) | none (six network settings under `HKLM`) |
| [Internet Connection Utility](#internet-connection-utility) | `bin\InternetConnectionUtility.exe` | 1.4.0.0 | asInvoker (prompts to relaunch) | none (repairs change system settings) |

Roughly 24,532 lines of C++ across the nine programs. Nothing to install, no
runtime to ship: the build links the compiler runtime statically, so each
executable depends only on DLLs that Windows itself provides.

---

## Quick start

The whole suite builds from one script at the root. It needs **MinGW-w64** and
nothing else — no MSVC, no `vcvars64.bat`, no Visual Studio.

```bat
build_all.bat
```

Output lands in `bin\`. To start from scratch (wipe `bin\` and the staging
area first):

```bat
build_all.bat /clean
```

To rebuild everything from scratch and package it in one go — the archive can
then only contain binaries built in that run:

```bat
release.bat /build
```

That writes `release\Zanphar-WinSuite-v<version>.zip`, with a manifest and
SHA-256 checksums — see
[release packaging](#release-packaging-releasebat). Run `release.bat` without
`/build` to package the existing `bin\` as-is.

If `g++` and `windres` are not on `PATH`, install the toolchain once:

```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

…or point the script at an existing toolchain for the current shell:

```bat
set "MINGW_BIN=C:\mingw64\bin"
build_all.bat
```

Requirements: 64-bit Windows 10/11, MinGW-w64 with `g++`, `windres` and
`objdump`. Verified with WinLibs GCC 16.1.0 (UCRT, POSIX threads).

---

## Repository layout

```
Zanphar's WinSuite\
├── .github\workflows\build.yml   CI: build, self-contained check, tag releases
├── build_all.bat                 suite build: static, self-contained, into bin\
├── release.bat                   release step: ZIP + manifest + SHA-256
├── VERSION                       suite version, used to name releases
├── README.md                     this file
├── CHANGES.md                    changelog for the suite and its programs
├── LICENSE.md                    BSD 3-Clause licence for the whole suite
├── tools\
│   ├── make_release.ps1          release worker (hashing, manifest, archive)
│   └── verify_binaries.ps1       checks that bin\ imports only Windows DLLs
├── bin\                          build output (created by build_all.bat)
│   ├── BrowserCleaner.exe
│   ├── DNSProtect.exe
│   ├── SmartRAM.exe
│   ├── SystemTurbo.exe
│   ├── HardWin.exe
│   ├── PCSpeedTest.exe
│   ├── HomePolicy.exe
│   ├── InternetTuningWizard.exe
│   ├── InternetConnectionUtility.exe
│   ├── ZanpharsWinSuite.exe      the suite window
│   └── WinSuiteAgent.exe         sign-in helper for saved task rules
├── release\                      release output (created by release.bat)
│   ├── Zanphar-WinSuite-v<version>\        the package: exes, docs,
│   │                                        MANIFEST.txt, SHA256SUMS.txt
│   ├── Zanphar-WinSuite-v<version>.zip     the package, zipped
│   └── Zanphar-WinSuite-v<version>.zip.sha256
│
├── BrowserCleaner\          Browser Cleaner — anti-tracking cleaner
│   ├── build.bat                 MinGW build for this program only
│   ├── src\main.cpp              the entire program (1,288 lines)
│   ├── src\resources.rc          icon + version information
│   ├── src\BrowserCleaner.ico    application icon
│   ├── src\icon_preview.png      icon preview
│   └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── DNSProtect\              DNS Protect — DNS hijack watchdog
│   ├── README.md                 program documentation
│   ├── LICENSE.md                BSD 3-Clause licence (program)
│   ├── build_mingw.bat           MinGW build for this program only
│   ├── CMakeLists.txt            CMake build (target: dnsprotect)
│   ├── .clang-format             house style
│   ├── .gitignore                build artifacts
│   ├── src\                      app, ui, dns, config, theme, log, common.h
│   ├── resources\                app.ico, app.manifest, resources.rc
│   └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── SmartRAM\              Smart RAM — floating memory widget
│   ├── README.md                 program documentation
│   ├── CMakeLists.txt            CMake build (target: SmartRAM)
│   ├── src\                      main, app, widget, settings_win, layerwin,
│   │                             settings, metrics, cleaner, tray, gfx
│   ├── res\                      app.ico, app.manifest, app.rc
│   └── tools\                    build.ps1         MSVC one-shot build
│                                 auto_test.ps1     behavioural test run
│                                 clean_test.ps1    memory-release test
│                                 settings_test.ps1 settings card test
│                                 tray_test.ps1     tray menu test
│                                 tray_live.ps1     tray animation capture
│                                 tray_locate.ps1   tray icon coordinates
│                                 screenshot.ps1    window capture helper
│                                 make_icon.ps1     icon pipeline (GDI+)
│                                 shot_final.png,
│                                 shot_settings.png,
│                                 shot_toast.png    reference screenshots
│
├── SystemTurbo\             System Turbo — reversible tweaks + monitor
    ├── README.md                 program documentation
    ├── LICENSE.md                BSD 3-Clause licence (program)
    ├── build_mingw.bat           MinGW build for this program only
    ├── CMakeLists.txt            CMake build (target: systemturbo)
    ├── .clang-format             house style
    ├── .gitignore                build artifacts
    ├── src\                      app, ui, turbo, ram, config, theme, log,
    │                             common.h, icon_preview.png
    ├── resources\                app.ico, app.manifest, resources.rc
    └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── HardWin\                 HardWin — reversible hardening, tuning, recovery
│   ├── README.md                 program documentation
│   ├── build_mingw.bat           MinGW build for this program only
│   ├── .clang-format             house style
│   ├── .gitignore                build artifacts
│   ├── src\                      harden (catalog, audit, journal), main (window,
│   │                             --audit, --selftest)
│   ├── res\                      app.manifest, app.rc, app.ico,
│   │                             icon-preview.png
│   └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── PCSpeedTest\             PCSpeedTest — CPU, memory, disk and network speed
    ├── README.md                 program documentation
    ├── build_mingw.bat           MinGW build for this program only
    ├── .gitignore                build artifacts
    ├── src\                      pcspeedtest (catalog, measurements, machine,
    │                             CSV, self-test), main (window, --run, --selftest)
    ├── res\                      app.manifest, app.rc, app.ico,
    │                             icon-preview.png
    └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── HomePolicy\              Home Policy — Group Policy Editor for Home editions
    ├── README.md                 program documentation
    ├── build_mingw.bat           MinGW build for this program only
    ├── src\                      catalog (policy table, registry engine,
    │                             backup, self-test), main (window, --list,
    │                             --selftest)
    ├── res\                      app.manifest, app.rc, app.ico,
    │                             icon-preview.png
    └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
├── InternetTuningWizard\    Internet Tuning Wizard — three-step network tuning
│   ├── README.md                 program documentation
│   ├── build_mingw.bat           MinGW build for this program only
│   ├── .gitignore                build artifacts
│   ├── src\                      tuning (adapters, values, apply/restore,
│   │                             self-test), main (window, --list, --selftest)
│   ├── res\                      app.manifest, app.rc, app.ico,
│   │                             icon-preview.png
│   └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
│
└── InternetConnectionUtility\  Internet Connection Utility — connection doctor
    ├── README.md                 program documentation
    ├── build_mingw.bat           MinGW build for this program only
    ├── src\                      ui (GDI+ interface), doctor (checks,
    │                             scoring, repairs), net (ICMP, DNS, WLAN,
    │                             WinHTTP), main (entry point)
    ├── res\                      app.manifest, app.rc, app.ico,
    │                             icon-preview.png
    ├── docs\                     screenshots used by the README
    └── tools\make_icon.ps1       reproducible icon pipeline (GDI+)
```

---

## The suite build (`build_all.bat`)

One script, six stages, no MSVC anywhere in the chain.

1. **Locate the toolchain.** `MINGW_BIN` first, then the first `g++` on
   `PATH`, then a WinGet/WinLibs installation, then the usual `mingw64`
   directories. It insists on `g++.exe`, `windres.exe` and `objdump.exe`
   before going further.
2. **Stage outside the sources.** Objects, resources and generated manifests
   go to `%TEMP%\Zanphar-WinSuite-build` (falling back to `%LOCALAPPDATA%`,
   `Windows\Temp`, `C:\`). The path must be free of spaces — see
   [toolchain notes](#toolchain-notes--troubleshooting) for why.
3. **Build each project.** Sources (`src\*.cpp`) and the resource script are
   discovered per folder; every project is compiled with
   `-std=c++17 -O2 -municode -mwindows -static -s` and the per-project link
   libraries from the recipe table inside the script.
4. **Copy into `bin\`.** Only finished executables end up there; no
   intermediate files land in the project folders.
5. **Build the suite launcher.** `build.bat` links `bin\ZanpharsWinSuite.exe`
   from `src\` and runs its `--selftest`; a failing self-test fails the run. A
   `/clean` build (and therefore `release.bat /build`) always leaves a tested
   launcher beside the other programs. Checks that describe the machine rather
   than the program — a configured network adapter, a third-party startup
   service — report `[SKIP]` where a machine has none, so the same self-test
   passes on a clean CI runner and on a desktop. The run ends with one line
   carrying all three counts, `RESULT: 62 passed, 0 skipped, 0 failed.`, so a
   CI log can be scanned — or grepped for `RESULT:` — without counting
   `[PASS]` lines by hand.
6. **Verify.** `objdump` reads each output's import table; the run fails if a
   MinGW runtime DLL (`libgcc`, `libstdc++`, `libwinpthread`) leaked in.
   Exit code `0` = every project built and is self-contained, `1` otherwise.

`/clean` deletes `bin\` and the staging area first. `/help` prints usage.

### Per-project builds

Each program can still be built on its own:

| Program | Command |
|---|---|
| Browser Cleaner | `BrowserCleaner\build.bat` |
| DNS Protect | `DNSProtect\build_mingw.bat`, or CMake (`dnsprotect`) |
| Smart RAM | `SmartRAM\tools\build.ps1` (MSVC) or CMake (`SmartRAM`) |
| System Turbo | `SystemTurbo\build_mingw.bat`, or CMake (`systemturbo`) |

CMake builds work with either toolchain:

```bat
cmake -S . -B build
cmake --build build --config Release
```

Note: the two `build_mingw.bat` scripts pass their manifest override with
g++'s `-B` prefix and do not survive a space in the folder path, so inside
`Zanphar's WinSuite\` they fail where `build_all.bat` succeeds. Use the suite
script here, or move the checkout to a space-free path.

---

## Release packaging (`release.bat`)

Turns `bin\` into one distributable archive. `release.bat /build` rebuilds
the suite from a clean state first, so the archive cannot be made from stale
binaries; plain `release.bat` packages whatever is already in `bin\`.

```bat
release.bat /build          # clean rebuild + package (one command)
release.bat                 # package the current bin\ as-is
release.bat /version 0.3.0  # optional version override
```

### What it does

1. Reads the version from the `VERSION` file at the root (`/version` wins if
   given) and validates it.
2. With `/build`: runs `build_all.bat /clean`, which wipes `bin\` and the
   staging area and rebuilds every program. A non-zero build exit stops the
   run here — nothing is packaged from a failed or partial build.
3. Stages `bin\*.exe` plus `README.md`, `CHANGES.md` and `LICENSE.md` into
   `release\Zanphar-WinSuite-v<version>\`.
4. Writes `MANIFEST.txt`: per program its product name, file name, file and
   product version, exact size, description, publisher and full SHA-256 —
   all read from the version resource inside each executable, not copied from
   a hand-maintained table.
5. Writes `SHA256SUMS.txt`, one SHA-256 per packaged file (LF line endings,
   two-space separator, so `sha256sum -c` accepts it).
6. Zips the package and writes `<archive>.zip.sha256` for the archive itself.
7. Removes every older `Zanphar-WinSuite-v*` package, archive and `.zip.sha256`
   from the release folder, so it always describes exactly one version.
   Nothing outside that name pattern is deleted. Before anything is staged,
   each older package is probed for open files; if one is in use (open in
   Explorer, held by a scanner) the run stops immediately, names the busy file
   and writes nothing, so a doomed release cannot leave the folder half
   updated. The removal itself runs after this version's three artefacts are
   written, and a lock that appears in between fails the run and names the
   stuck items rather than reporting success while two versions sit side by
   side.

### Output

| Path | Contents |
|---|---|
| `release\Zanphar-WinSuite-v<version>\` | staged package: every executable from `bin\`, the three docs, `MANIFEST.txt`, `SHA256SUMS.txt` |
| `release\Zanphar-WinSuite-v<version>.zip` | the same package, with the versioned folder as the top-level entry |
| `release\Zanphar-WinSuite-v<version>.zip.sha256` | `hash *archive.zip`, verifiable with `sha256sum -c` |

### Verifying a download

```bash
sha256sum -c Zanphar-WinSuite-v2.0.0.zip.sha256   # the archive
sha256sum -c SHA256SUMS.txt                       # the contents, inside the package
```

`certutil -hashfile <file> SHA256` and
`Get-FileHash -Algorithm SHA256 <file>` produce the same digests without
coreutils.

### Behaviour worth knowing

- **Exit codes:** `0` on success, `1` on any failure (no `bin\`, missing or
  invalid version, no PowerShell, script missing, packaging error, or — with
  `/build` — a build that did not succeed).
- **Freshness is structural, not best effort.** `/build` deletes `bin\`
  before compiling, so every executable in the archive was produced by that
  run; if the build fails, the run stops before the packaging step instead of
  shipping the previous output.
- **Per-version, not all-or-nothing:** re-running replaces only the artifacts
  of the same version; packages of other versions in `release\` are left
  alone, so several releases can coexist while you compare them.
- **No per-program registration.** Any `*.exe` in `bin\` is packaged, so a
  program added to the suite is released without touching the release step —
  its names, version and description come from its own `.rc` block.
- **Portable archives.** Entries are written with forward slashes; both
  `Compress-Archive` and `ZipFile::CreateFromDirectory` on Windows PowerShell
  5.1 store backslashes, which makes macOS/Linux extractors create one file
  literally named `folder\program.exe`.
- **Clean failures.** A failed run removes its own half-written archive and
  leaves other versions in `release\` untouched.

### Versioning

The suite is versioned as a bundle; each program keeps its own version in
its `.rc` `VERSIONINFO` block. Together they mean:

| Where | What | Read by |
|---|---|---|
| `VERSION` | suite version, e.g. `2.0.0` | `release.bat` (archive name, manifest header) |
| `<project>\**\*.rc` | program version, e.g. `1.0.0.0` | Windows, `release.bat` (per-program rows) |

`tools\make_release.ps1` does the hashing, manifest and archive work;
`release.bat` is the entry point and holds only the argument handling.

---

## Continuous integration

[`.github\workflows\build.yml`](.github/workflows/build.yml) builds and
packages the whole suite on a clean Windows runner — every branch push, every
pull request, and on manual dispatch. A `v*` tag push additionally publishes
the packaged archive as a GitHub release, and a manual run can do the same for
a tag that already exists — see
[Releasing a version](#releasing-a-version).

| Step | What it does |
|---|---|
| Checkout | `actions/checkout@v7` |
| Toolchain | `msys2/setup-msys2@v2`, environment `UCRT64`, package `mingw-w64-ucrt-x86_64-gcc` |
| Locate | reads the action's `msys2-location` output, checks that `g++`, `windres` and `objdump` are there, exports `MINGW_BIN`, the variable `build_all.bat` looks at first |
| Build + package | `release.bat /build` — the same single command used locally: `build_all.bat /clean` wipes `bin\` and the staging area, then every program is built, then the archive is written |
| One version only | fails unless `release\` holds exactly one package folder, one archive and one checksum, all named after the version in `VERSION` — so a pulling step cannot publish out of a folder holding a second version |
| Docs match | hashes the packaged `README.md`, `CHANGES.md` and `LICENSE.md` against the repository copies and fails unless they are byte-identical, so a download never describes a different release from the source |
| Licence | reads the packaged `LICENSE.md` and fails unless it names the BSD 3-Clause License, carries the `Copyright (c) 2026-2027, Charles McDonald` line, still has all three clauses and the warranty disclaimer, and contains no MIT permission grant - so a release can never ship under the wrong licence or the wrong holder |
| Version resource | reads the version resource of every packaged `.exe` and fails unless `CompanyName` is exactly `Charles McDonald` and the copyright line names him too - the strings Windows shows under Properties → Details, so a binary built from a stale or missing `.rc` cannot ship |
| Verify | `tools\verify_binaries.ps1 -BinDir bin` |
| Artifacts | `bin\*.exe` as `Zanphar-WinSuite-bin-x64`, and `release\*.zip` + `.zip.sha256` as `Zanphar-WinSuite-release-zip`, both kept 30 days |

CI runs the release step rather than `build_all.bat` on its own, so every
push proves the whole path a maintainer would run: clean build, packaging,
manifest and checksums. The step fails if a project does not compile, if
`build_all.bat`'s own `objdump` pass finds a MinGW runtime DLL, or if
packaging fails — and because `/build` empties `bin\` first, no archive can
contain a binary from an earlier run. The verification step then repeats the
DLL check independently against an allowlist of Windows-provided DLLs.

### Releasing a version

The suite is published by tagging the commit that has the release already
committed in `VERSION`:

```bash
# VERSION says 2.0.0, and everything is committed
git tag v2.0.0
git push origin v2.0.0
```

That tag runs a second job, `release`, which only starts after `build` has
succeeded and only for a tag ref or a manual run that named a tag:

| Step | What it does |
|---|---|
| Checkout | `actions/checkout@v7` at the tag |
| Archive | `actions/download-artifact@v8` — the ZIP and `.zip.sha256` the build job uploaded, into `release\` |
| Tag check | strips the leading `v`, rejects a version that is not `[0-9][0-9A-Za-z.-]…`, and fails if the tag disagrees with `VERSION` |
| Release notes | takes the `## [<version>]` section of `CHANGES.md`, up to the next `##` heading, and writes it to `release-notes.md`; a missing or empty section fails the job |
| Checksum | re-hashes the archive and compares it with the `.sha256` file that is about to be uploaded |
| Publish | `gh release create <tag> <zip> <zip.sha256> --verify-tag --notes-file release-notes.md` |

What that means in practice:

- **Nothing is built or packaged twice.** The release job publishes the ZIP
  the build job produced and uploaded in the same run, so the released
  archive is exactly the one whose binaries passed the self-contained check,
  and a tag can never ship whatever `.exe` or old archive happened to be on a
  runner.
- **The tag and `VERSION` must agree.** Bump `VERSION`, commit, then tag. A
  `v2.0.1` tag against `VERSION` `2.0.0` fails in the tag-check step before
  anything is published, instead of attaching an archive whose name and
  manifest contradict the tag.
- **The release body is the changelog.** The notes are the version's own
  `CHANGES.md` section — heading excluded, everything down to the next `##`
  — so the release page and the changelog cannot drift apart. GitHub's
  auto-generated notes are not used. The section must exist (`## [2.0.0]`,
  with or without a date) and must not be empty; tagging 0.3.0 while the
  newest heading is `## [Unreleased]` fails before anything is published.
- **Re-running the pipeline is safe.** If the release already exists, the
  assets are re-uploaded with `--clobber` and the body is refreshed with
  `gh release edit --notes-file`, instead of the job failing.
- **Suffixed versions are prereleases.** `v0.3.0-rc1` is created with
  `--prerelease`, so GitHub does not mark it "Latest".
- **Least privilege.** The build job keeps `contents: read`; only the release
  job asks for `contents: write`, and it is skipped entirely for pull
  requests, so a PR run never holds a write token.
- **Attached files:** the archive and its `.sha256`. The per-file
  `SHA256SUMS.txt` and `MANIFEST.txt` travel inside the archive.
- **No signing.** The release is unsigned; the checksum only proves the
  download is intact.

### Publishing an existing tag

A tag that was pushed before the release job existed — or a release deleted
by hand — is published by a manual run that names the tag, without touching
the tag itself:

```bash
gh workflow run build.yml -f tag=v2.0.0
```

The same thing in the browser: **Actions → build → Run workflow**, branch
dropdown as it is, `tag` field set to `v2.0.0`. The `tag` input is the only
one; leaving it empty builds and packages the selected ref and publishes
nothing, which is what the dispatch did before this input existed.

The run behaves exactly like a tag push: both jobs check out that tag (not
the selected branch), build it, verify it, package it, and then publish the
archive with the changelog section as the body. Because a release may already
exist for that tag, the publish step uploads with `--clobber` and refreshes
the body with `gh release edit`. The usual guards still apply — the tag must
be `v` plus a version, `VERSION` at that commit must match, and `CHANGES.md`
at that commit must have the section — so publishing an old tag can fail on a
changelog written after the fact; fix it in a new tag instead.

### Verifying locally

The same script runs on a developer machine:

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify_binaries.ps1
```

It finds `objdump` through `MINGW_BIN`, then `PATH`, then the usual MinGW
locations. `-BinDir` points it at another folder and `-Expected A.exe,B.exe`
additionally requires those outputs to exist. Every `*.exe` in the folder is
inspected: the Windows DLLs on the allowlist and the `api-ms-win-*` API sets
pass, anything else — `libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
`libwinpthread-1.dll`, a third-party runtime — fails the run with exit code
`1` and names the DLL.

---

## The suite's pages

`bin\ZanpharsWinSuite.exe` is the primary interface: fifteen pages in one
window, one visual language, one elevation prompt. The four programs' engines
run inside it, so nothing has to be started separately.

| Page | What it does | Engine from |
|---|---|---|
| Dashboard | RAM, CPU, disks and the quick jobs. The **Storage** card lists every connected volume — installed, external, removable, optical and network — and scrolls through all of them when there are more than fit, with a slim position bar | suite |
| Cleanup | measures and deletes temporary files, Windows Update, prefetch and crash caches; your Downloads folder is listed but stays unticked until you choose it, asks before it takes anything, and what it clears goes to the Recycle Bin instead of being deleted. The **Recycle Bin** itself is a row too: measured across every drive through the shell, unticked by default, emptied only after a confirmation that names the item count and says the deletion is permanent, and never armed by Select all or a saved tick. Your ticks are remembered between launches, except for Downloads and the Recycle Bin, which never return ticked. **Select all** and **Clear all** tick or clear the list in one press (Select all leaves Downloads alone), and a **Reset to defaults** link puts the selection back to stock. The page measures itself the first time you open it and fills the rows in as each one is measured (a long scan can be cancelled and keeps what it measured so far, with the earlier figure shown dimmed and marked on rows it never reached), lists the biggest folder first once a pass lands (an **Order** switch above the list flips it back to the built-in order and is remembered between launches, and rows with nothing to compare stay below the figures), and after a clean the rows are rebuilt and re-measured on their own, so Scan stays available and the list still shows real leftovers | suite |
| Browsers | per-browser history, cookies, autofill, caches and session data: measured before anything is deleted, a running browser is skipped, and its windows can be closed first | Browser Cleaner |
| Memory | live figures, regular and deep clean, plus Smart RAM's automatic clean: threshold chips (65/75/85/95 %), idle clean, clear clipboard. Both manual cleans ask first unless their prompts are switched off in Settings | suite + Smart RAM |
| DNS | every configured adapter with its resolver, one provider applied to all of them, system DNS restored, and a 1.5 s watchdog that reverts interference. The adapter list scrolls a whole card per wheel notch and ends on a card edge, with a slim bar and a "10 adapters - scroll for all" count whenever the list runs past the bottom, so no adapter is out of reach | DNS Protect |
| Networking | four latency-first Windows settings for gaming: Nagle's algorithm off on every configured adapter, network throttling and system responsiveness off, the Games multimedia task profile, and no QoS reservation. Each row has its own switch, **Apply profile** and **Revert all** work over the same journal, and every value is saved with its type before it changes, so Revert puts back exactly what was there; a value another tool set reads **Turn off**, which saves the value with its type before removing it, so the row returns to the Windows default and **Revert** restores exactly what the other tool had set - no row is read-only. **Restore Windows defaults** does that for all four at once: one press switches every tweak off, journalling each value first, so **Revert all** still puts back what another tool had chosen. The button is only armed while at least one tweak is on, and a second press simply reports that everything is already at the Windows default. A **Latency check** card takes the before/after measurement the rows are supposed to earn: type any host or IPv4 address (1.1.1.1 to start), press **Measure before** (or Enter in the field), apply what you like, then **Measure after** - four echoes each time on a worker so the window never freezes, then both averages with their min-max ranges and the change as one span - "11 ms to 14 ms - 3 ms higher on average" - with a packet-loss note when it moved, green when the average fell and amber when it rose, and **Reset** starting the comparison over. **Apply profile** with no reading yet takes the baseline itself before it writes anything, then applies by itself and finishes by taking the after-reading too, so the comparison appears without a second press; an apply that changed nothing does not ping, a baseline already in hand is never replaced, and a failed ping leaves the error on the card without cancelling the apply. Editing the host drops the readings, because a number only means something against the host it was taken on | suite |
| Game Booster | System Turbo's profile as the first row (High performance plan, parked background services, Game DVR off), then the tried-and-true gaming tweaks applied by **Apply profile**: Game Mode on, mouse acceleration off, fullscreen optimisations off, window animations and transparency off. The advanced rows — GPU scheduling (HAGS), power throttling off, Win32 priority separation 38 — are individual-only, never part of the profile, and say what they need. Every value is saved with its type before it changes, so **Revert all** puts back exactly what was there. Every row is switchable from this page: a value another tool already set reads **Turn off** rather than being left alone — the current value is journalled and then cleared back to the Windows default, so **Revert** still restores it. Game Mode is the exception, because its Windows default is *on*: the row reads ON until it is switched off, and switching it off writes 0 instead of removing the value. **Apply profile** reports any setting Windows refused instead of calling the profile done, and **Restore** keeps the record of a service it could not put back - start type and delayed-start flag included - so a retry as administrator finishes the job. Below the tweaks sits the **background services** list: twenty-eight rows for the services a gaming session does not need - telemetry, the touch keyboard panel, Netlogon, Xbox Live (four services), image acquisition, geolocation, the compatibility assistant, WAP push routing, retail demo, Error Reporting, Print Spooler, certificate propagation, enterprise app management, biometrics, the Insider service, the connection broker, Remote Desktop (three services), the portable device enumerator, Parental Control, Connect Now, smart cards (three services), Event Log, Remote Registry, the phone service, downloaded maps, AllJoyn, Fax and Secondary Logon - each showing the real service names with their live start type and state. **Turn off** stops them and sets them to Manual, not Disabled, after saving the start type, running state and delayed-autostart flag; **Revert** puts each back exactly, **Revert all** includes them, and rows Windows leans on print their state in the warning colour. None of them is part of the profile, and a row Windows already had off reads **Already off** rather than offering a switch that would do nothing | suite + System Turbo |
| Large files | find big files and send them to the Recycle Bin | suite |
| Startup | turn startup entries on and off | suite |
| Deactivate | park third-party services and put them back | suite |
| Services | every installed Windows service with its start type and state, **Start** and **Stop**, the four start types as chips (Automatic, Automatic (delayed), Manual, Disabled), and **New…**, **Edit…** and **Delete** for entries whose binary is not a Windows component; an **Edit**/`Delete` on one of those rows is a full create/edit/remove of the entry, and Windows components are re-typed but never edited or deleted from here. A **Search services by name** box above the list narrows it as you type, matching the display or the service name regardless of case, with a × to clear it and Escape to clear then leave the box. Rows carry a tick: press one or more — or **Select all** above the list, limited to what the filter shows — and **Start**/**Stop** work on every ticked row that needs it, with **Clear ticks** to empty the set. A single unticked row still works the old way: it is highlighted and the buttons act on it. The list scrolls a whole row per wheel notch and its bottom edge lands on a row boundary, so a name is never left half visible at the bottom | suite |
| Task Manager | every process on the PC with its PID, account, affinity, priority and memory, with a **Search processes by name** box that narrows the list as you type. **End task** sends WM_CLOSE to its windows, **Force end** terminates it after a confirmation, **New task…** starts a program or command line. Every row has a tick, so **End task** and **Force end** can take one process, several, or everything **Select all** ticks, and report how many went and which refused; the card's cores and priority still belong to the highlighted row. The selected process's cores are chips (one per logical CPU) with **Apply cores**, its priority is a class chip with **Apply class**, and **Remember these for this program, at sign-in too** saves the pair as a rule for that executable — applied at once, at every sign-in by `WinSuiteAgent.exe`, and removable with **Forget the saved rule**. Like the service list, it scrolls by whole rows and ends on a row boundary, so no process row is cut off at the bottom | suite |
| System | hardware and operating-system readout, including a **Storage devices** list of every connected volume that scrolls through all of them rather than cutting the list off | suite |
| Storage | every local volume with the media type Windows reports (the seek-penalty answer), its free space, and the mode Windows' own defragmenter will use: **Defragment** (`/D`) on a mechanical disk, **TRIM** (`/L`) on a solid-state drive, and a disabled **Unavailable** where the media type could not be read. A per-volume confirmation names the drive and mode before the elevation prompt; while the worker runs, every Storage action is disabled, elapsed time and reported percentage are shown, and **Cancel** terminates the child process. The result or the defragmenter's own error is reported on the page | suite |
| Settings | **Close to system tray** and **Minimize to system tray** (both off until you choose them, remembered in `settings.ini`), **Show on system tray at startup** (opens straight into the tray with no window on the desktop; if the shell refuses the icon the window is shown instead, so the suite can never run with nothing to click), and **Toast notifications** (on by default, gating the tray balloon a menu-started clean reports itself with - a settings file without the key reads as on). Then **Ask before Regular clean** and **Ask before Deep clean** (both on by default, so a clean never runs unannounced), the copyright line and a **Read the licence** button that opens the full BSD 3-Clause agreement read-only, with wheel scrolling and Close/Escape to leave it. The tray icon's menu carries **Open**, **Regular Memory Clean**, **Deep Memory Clean** and **Exit**, so a clean can be run without opening the window. The page scrolls on a short window | suite |

The window opens at **1024 x 768** of layout, centred on the screen it starts
on, and that size is scaled with the display - 1280 x 960 at 125%, 1536 x 1152
at 150% - so a high-DPI screen gets the same room instead of a squeezed layout.
The same layout renders at any size from 980 x 640 upwards, tracked minimum
included: the sidebar tightens its rows as the window shrinks and, once
fifteen entries no longer fit, scrolls - the wheel over the sidebar moves the
list and a slim bar shows where it is, so every page stays reachable on a short
window. Pages that change the machine
(Browsers, DNS, Networking, Game Booster, Startup, Deactivate, Services, Storage, and the task
actions Windows refuses to a standard user) ask for elevation once and restart elevated,
rather than running at reduced rights.

Building is unchanged: `build_all.bat` builds every program and then the suite
through `build.bat`. The standalone executables still exist on their own.

---

## Program reference

### Browser Cleaner

`bin\BrowserCleaner.exe` — inspired by IObit's *Browser Anti-Tracking*
(`BrowserCleaner.exe`).

Removes tracking data from Internet Explorer (history, cookies, cache and
session recovery), Chromium-based browsers (Chrome, Edge, Brave, Vivaldi,
Opera, Opera GX) and Firefox (history, cookies, autofill forms, cached files
and session restore). IE is read from WinINet's shared Windows folders (the
dedicated History folder, INetCache and INetCookies) plus its recovery folder;
WebCache is deliberately not deleted because it also holds cookies and other
WinINet state. IE has no Chromium-style profile, so those traces are resolved
from LocalAppData/RoamingAppData directly. It detects running browsers and
offers to close them first, then reports what was measured and deleted, per
browser and per trace item.

- **Storage engine** — inspect/delete trace files via `std::filesystem`,
  including the SQLite sidecars (`-journal`, `-wal`, `-shm`) that hold the
  same tracking data.
- **Process module** — Toolhelp32-based detection and graceful close of
  running browsers.
- **Catalog** — browser and trace-item definitions with per-browser accents.
- **UI** — GDI+ dark theme, per-monitor-v2 DPI aware.
- Single source file, `src/main.cpp` (1,288 lines); no settings file, no
  elevation, no background service.

### DNS Protect

`bin\DNSProtect.exe` — inspired by the *DNS Protection* helper in
Advanced SystemCare. Full details in
[DNSProtect/README.md](DNSProtect/README.md).

- Enforces a trusted DNS configuration on every IP-enabled adapter: the
  current system settings, a public provider (Google, Cloudflare, OpenDNS,
  Quad9, Comodo, Verizon, DNS Advantage) or a custom pair.
- A watchdog re-reads the per-adapter registry values (`NameServer`,
  `DhcpNameServer`) and snaps unauthorized changes back within ~1.5 s,
  counting and logging every incident.
- Dark, DPI-aware dashboard with live adapter list; minimizes to the tray.
- Requests elevation through its manifest and self-relaunches; declining the
  UAC prompt leaves it in read-only monitoring mode.
- Log and settings: `%ProgramData%\DNSProtect\dnsprotect.log` and
  `dnsprotect.ini`.
- Modules: `app` (entry, single instance, elevation), `ui` (dashboard, tray),
  `dns` (watchdog, apply/restore, baseline), `config` (providers, INI),
  `theme`, `log`, `common.h`.
- Limits: classic IPv4 stack only; snapshot-and-restore rather than
  kernel-level write blocking; static DNS changes are nudged with
  `DhcpNotifyConfigChange` + `ipconfig /flushdns`.

### Smart RAM

`bin\SmartRAM.exe` — inspired by IObit's `Suo10_SmartRAM.exe` floating
"Monitor and release memory" widget. Full details in
[SmartRAM/README.md](SmartRAM/README.md).

- Floating monitor card: animated RAM gauge (cyan → red), used/total, free
  memory, CPU load and process count, refreshed twice a second.
- **Clean now** purges the standby list
  (`NtSetSystemInformation(SystemMemoryListInformation)`) and trims every
  process working set (`EmptyWorkingSet`), then shows `1.24 GB FREED` on the
  card under the button until the next clean. No toast, no tray balloon — the
  result never pops up as a notification.
- Animated tray icon that eases with the gauge and spins while cleaning;
  tooltip shows RAM % / CPU %.
- **Smart Clean** at a configurable 50–95 % threshold, **Clean When Idle**
  after 5 minutes of no input, **Clear Clipboard** on clean, **Always on
  Top**, **Start with Windows** (HKCU `Run`).
- Per-pixel-alpha layered windows (`UpdateLayeredWindow`) with Direct2D /
  DirectWrite rendering; per-monitor-v2 DPI aware; drag position remembered
  with per-monitor bounds checking; single instance.
- Settings: `%LOCALAPPDATA%\SmartRAM\config.ini`.
- Modules: `main`, `app`, `widget`, `settings_win`, `layerwin`, `settings`,
  `metrics`, `cleaner`, `tray`, `gfx`, plus `comptr.h` / `util.h`.

### System Turbo

`bin\SystemTurbo.exe` — inspired by the *Turbo Boost* and *Hardware
Accelerate* modules in Advanced SystemCare. Full details in
[SystemTurbo/README.md](SystemTurbo/README.md).

- **Reversible boost**: switches to the High Performance power plan, parks a
  small set of safe services (Windows Search, SysMain, Print Spooler,
  Bluetooth Support, Diagnostic Policy, Fax, WMP Network Sharing) in
  manual-start mode while they are stopped, and disables Game DVR background
  recording. Original values are captured in persisted restore records, so a
  boost survives a restart and is undone exactly on power-off.
- **Resizable dashboard**: cards and controls follow the client area and the
  window can be maximized, with a DPI-scaled 660 x 752 client floor so the
  layout and hit targets never collapse.
- **Live card**: RAM load, free bytes, CPU load and the heaviest images
  (summed per executable), colour-coded by share of RAM.
- **Memory release**: one click or tray menu empties large working sets and
  purges the standby list; can run on boost and automatically at 80 % RAM
  load.
- Requests elevation through its manifest; declining UAC keeps
  instrumentation alive but rejects tweaks.
- Log and state: `%ProgramData%\SystemTurbo\systemturbo.log` and
  `systemturbo.ini`.
- Modules: `app`, `ui`, `turbo` (tweaks, restore records, worker), `ram`
  (probes, top processes, release), `config`, `theme`, `log`, `common.h`.

### HardWin

`bin\HardWin.exe` — hardening, tuning and recovery for Windows 10 and 11.
Full details in [HardWin/README.md](HardWin/README.md).

- **Fourteen checks in three groups.** Six security rows (SMBv1, LLMNR, AutoRun,
  SmartScreen, PowerShell script-block logging, Defender unwanted-app
  blocking), three performance rows (best-performance visual effects, the
  ten-second startup delay, menu delay) and five reliability rows (restart
  actions for Event Log, WMI and Defender, automatic maintenance, no forced
  restart with programs open, fast startup off, the automatic crash dump on).
- **A write is only called done once it reads back.** Every value is re-read
  after it is written; a row whose effective value did not take - a policy or
  another tool refusing it - is rolled back and reported instead of being
  counted as hardened. Anything refused, applied or restored is appended to
  `%LOCALAPPDATA%\HardWin\hardwin.log`, and one copy of the window at a time is
  enforced with a named mutex, so two instances cannot interleave writes on one
  journal.
- **Every change is journalled.** The original value, its type and whether the
  value existed at all are saved under the hive that owns it before anything is
  written, so **Revert** restores what was there - including deleting a value
  that was not there before, and the key when this tool created it.
- **Honest defaults.** A setting Windows already ships hardened (SmartScreen,
  automatic maintenance) reads as hardened when it is unset rather than
  offering to "fix" it; a row the process cannot reach says **needs
  administrator** instead of claiming the machine is open.
- **Every row is selectable on its own.** A tick box sits on each row (a click
  anywhere on the row ticks or unticks it), **Select all** and **Clear** sit at
  the end of the status line, and the line counts both readings - "3 of 12
  hardened - 2 selected". **Harden selected** and **Revert selected** touch only
  the ticked rows, in catalog order: hardening skips and counts the rows already
  done, reverting skips the rows with no saved value, and any refusal is named,
  so a selection is never reported as complete when a row was left alone. Ticks
  survive **Re-check** - the selection belongs to the list, not to the audit.
- **Two console modes.** `--audit` prints what every check reads and changes
  nothing; `--selftest` proves the catalog, the journal round-trip on a
  throwaway key, the read-back after a write and the service-action encoding,
  and prints one `RESULT:` line.
- The manifest is `asInvoker`: the three per-user rows need no elevation, and
  the window offers **Restart as administrator** for the rest.
- Modules: `harden` (catalog, audit, journal, service manager), `main` (window,
  console modes).

### PCSpeedTest

`bin\PCSpeedTest.exe` — fourteen figures for one machine, measured the same way
on every run. Full details in [PCSpeedTest/README.md](PCSpeedTest/README.md).

- **Five groups, fourteen rows.** Two processor rows (the same dependent integer
  and float loop on one thread and on every thread, Mops/s), two memory rows
  (64 MiB copy bandwidth in GB/s and the latency of a dependent load in ns),
  two disk rows (192 MiB written and flushed, then read back with
  `FILE_FLAG_NO_BUFFERING` so the cache cannot answer for the drive), six
  network rows: the adapter's negotiated link speed, loopback TCP throughput,
  gateway latency, internet latency with jitter and packet loss, and Cloudflare
  download and upload rates - and two graphics rows: a compute shader's
  multiply-add rate in GFLOP/s and a 64 MiB texture copy's bandwidth in GB/s,
  both timed on a Direct3D 11 device with the GPU drained before the clock is
  read.
- **The machine is named from what Windows will say, not what it claims.**
  Windows 11 keeps `ProductName` = `Windows 10 Pro` for compatibility, so the
  version word comes from the build number (22000 and up is 11), with edition,
  display version and UBR beside it; cores come from `RelationProcessorCore`
  rather than the thread count, and the architecture from `GetNativeSystemInfo`.
  The window draws it in three lines: hardware, graphics adapter with its
  driver, then platform.
- **Every figure is work over a clock.** Each row runs a fixed share of work
  inside `QueryPerformanceCounter` bounds - 0.35 s of CPU churn, 0.3 s of
  memcpy, a fixed set of ICMP echoes, a byte-capped HTTPS transfer - so a run
  is a rate, not a reaction to how busy the machine was beforehand.
- **A row that cannot be measured says why.** No reply to ICMP ("ICMP may be
  filtered"), no default gateway, a server that answered with an error, a
  socket refused, a disk that could not be written, no Direct3D device: the row
  comes back with the reason and the rest still measure. The window shows the reason
  where the figure would be; the CSV leaves that column empty rather than
  writing a zero, because zero is a measurement and "unavailable" is not.
- **The CSV is the point.** Every completed run of all fourteen appends one line
  to `%LOCALAPPDATA%\PCSpeedTest\results.csv`, with the timestamp, machine
  name, processor, physical cores, logical threads, architecture, RAM with the
  modules' speed and maker, the graphics adapter and its driver version, and the
  OS beside the figures so readings taken on different machines stay comparable,
  and RFC 4180 quoting so the file opens in any spreadsheet. A file written
  under the older header is rewritten in place on the next append - the old
  `cores` figure moves to `threads` and a row that said Windows 10 beside a
  22000+ build is corrected to Windows 11. Measuring one row from the window does not write a
  line, so the file never mixes partial runs with full ones. **View results**
  reads the file back as a window of its own - one card per run, newest first,
  each with the timestamp, the machine and the fourteen figures, a `-` where a
  column was left empty - and **Open in spreadsheet** hands the file to the
  default application from there. **Empty file** in that window removes every
  run after a confirmation that names the count and defaults to No, leaving the
  header so the next run appends to an empty file the reader still understands.
- **Two console modes.** `--run` measures all fourteen, prints them with their
  notes and appends one CSV line; `--selftest` proves the engine without
  touching the network - fourteen checks over the catalog, the units, the CSV
  escaping, the CPU loop, memory copy and latency, loopback TCP, a 48 MiB disk
  round trip, a synthetic results.csv written and read back through the
  viewer's own reader (quoted comma, quoted quote, one blank column), the
  migration of a file under the older header, a firmware memory table the test
  builds itself (offsets, empty slots, `mixed` makers), emptying a file down to
  its header, a compute dispatch that either reports a rate or says why there is
  no device, and the machine
  description - whose name must agree with the build number it reads - and ends
  in a `RESULT:` line. Those file checks parse temporary files of their own; the
  user's results.csv is never opened.
- The manifest is `asInvoker`: sockets, ICMP echoes and its own folder are all
  a standard user already has. Modules: `pcspeedtest` (catalog, measurements,
  machine description, CSV writer, self-test), `main` (window, console modes).

---

### Home Policy

`bin\HomePolicy.exe` — a Group Policy Editor for Windows 10 Home and Windows 11
Home. Full details in [HomePolicy/README.md](HomePolicy/README.md).

Home ships without `gpedit.msc`, but the components that read policies still
check the same `HKLM\SOFTWARE\Policies` values. Home Policy writes thirty-seven
of them across seven categories — Windows Update, privacy and telemetry, Start
and search, File Explorer, system and apps, security, and Edge — with one plain
sentence per row and three states: **Not configured**, **Enabled**, **Disabled**.

- **Staged, not immediate.** A choice marks the card and counts in the footer;
  nothing is written until **Apply changes**. **Discard** clears the staging,
  and every row can be handed back to Windows by choosing Not configured, which
  removes the value again.
- **One prompt, at the right moment.** The manifest is `asInvoker`, so the whole
  catalog reads as a standard user; Apply reopens the window elevated instead of
  failing row by row with Access denied. The staged rows travel with it on the
  command line (`--apply "index:state;..."`), and the new window waits for the
  closing one's single-instance lock rather than exiting with the list.
- **Backup.** A UTF-16 `.reg` of every catalog value that is configured, written
  from the window in one click.
- **Copyright in the footer.** A hairline rule above a faint
  `© 2026-2027 Charles McDonald` line, with the administrator link on the same
  row, so the credit is visible without competing with the settings.
- **One setting from a shell.** `--apply "Widgets board" disabled` resolves the
  title, writes that row, reads the registry back and prints before/asked/reads
  plus the raw DWORD, exiting non-zero unless the read-back matches; an
  ambiguous title lists the candidates instead of guessing.
- **Two console modes.** `--list` prints every setting with the state this PC
  reads and its full registry path; `--selftest` proves the catalog (no empty
  rows, no duplicate values, at least thirty settings), the three-state mapping,
  and an enable/disable/reset round trip on a scratch key under
  `HKCU\Software\ZanpharsWinSuite\HomePolicy\selftest`, which it removes
  afterwards.
- **Honest about Home.** Microsoft documents nearly every policy here for Pro,
  Enterprise and Education, and none for Home. The catalog was audited against
  the ADMX files in `C:\Windows\PolicyDefinitions`: the two update-deferral rows
  are tagged **Home ignores this** (deferral is Windows Update for Business),
  the retired Cortana and Copilot rows **Deprecated by Microsoft**, and the
  telemetry row is gone because Home documents `AllowTelemetry = 0` as equal
  to `1`. Paths, value names and hives were corrected to what those ADMX files
  declare, including the user-scope Explorer rows and Windows Script Host's
  `Settings\Enabled`.
- Modules: `catalog` (policy table, registry engine, backup, self-test), `main`
  (window, console modes). Roughly 2,208 lines.

### Internet Tuning Wizard

`bin\InternetTuningWizard.exe` — a three-step wizard for Windows 10 and 11 that
tunes six network settings Windows actually reads. Full details in
[InternetTuningWizard/README.md](InternetTuningWizard/README.md).

- **Step 1 — the connection.** Every connected adapter is listed with its
  driver, address and current resolvers — Ethernet, Wi-Fi, VPN and Hyper-V
  switches included — and the list scrolls when it outgrows the page, so no
  adapter is unreachable. The DNS row writes either the system default or
  Cloudflare, Google or Quad9 for that adapter only — as both `NameServer` and
  the DHCP-side `DhcpNameServer`, with the previous pair saved so Restore puts
  the lease value back.
- **Step 2 — the whole PC.** Four switches, each with the sentence that says
  what it costs: network throttling off (`NetworkThrottlingIndex`
  `= 0xFFFFFFFF`), IPv6 off (`DisabledComponents = 0xFF`), the multimedia CPU
  reserve off (`SystemResponsiveness = 0`) and TIME_WAIT capped at 30 seconds
  (`TcpTimedWaitDelay = 30`).
- **Step 3 — review.** The page lists exactly what Apply will write, including
  the DNS provider, before any prompt appears. After Apply, the result page
  shows one green or red row per value, with Back to the review, Restore Windows
  defaults to undo the run, and Close.
- **One prompt, and a real report.** Apply relaunches the wizard elevated with
  the plan on its command line; the elevated copy writes, reads back and
  reports through a small text file, so the window shows a green or red row per
  value instead of an Access denied.
- **Restore Windows defaults.** Two clicks remove every value the wizard
  manages, returning the machine to "no value at all" for all four.
- **Two console modes.** `--list` prints every adapter with its resolvers and
  what the six settings currently read; `--selftest` proves the provider table,
  the plan round trip, the review text, the registry engine and the adapter
  enumeration.
- Modules: `tuning` (adapters, values, apply/restore, report file, self-test),
  `main` (wizard window, console modes). Roughly 2,244 lines.

### Internet Connection Utility

`bin\InternetConnectionUtility.exe` — a connection doctor for Windows 10 and 11 that measures why
an internet connection is failing, dropping or slow, scores what it finds and repairs what it can.
Full details in
[InternetConnectionUtility/README.md](InternetConnectionUtility/README.md).

- **One scan, about thirty checks.** Winsock and stack health, adapter and link state, DHCP and
  addressing, gateway reachability and route metric, interface errors, per-server DNS probes,
  ICMP latency/jitter/loss, the TCP :443 handshake, HTTPS egress, captive-portal detection, a
  short throughput sample, retransmissions and path MTU. Results stream
  into the list as each
  probe finishes, then become a 0–100 score.
- **Wi-Fi diagnostics.** The associated network, nearby networks with signal, band and channel,
  and 2.4 GHz channel occupancy, so interference-driven dropouts are visible rather than guessed
  at.
- **Windows tuning checks.** TCP receive-window auto-tuning, NIC power management, firewall
  profiles, core network services, and the user proxy/PAC configuration.
- **Nineteen repairs.** Flush and re-register DNS, renew DHCP, switch the adapter to
  Cloudflare/Google DNS or back to DHCP, reset Winsock/TCP/IP and auto-tuning, disable the user
  proxy or reset the WinHTTP proxy, stop the adapter powering down, maximise the Wi-Fi power plan,
  reset the interface metric, match the MTU to the measured path, enable or restart the adapter,
  reconnect Wi-Fi, start stopped network services and restore the hosts file. Each is labelled
  `ADMIN`, `CHANGES` or `RESTART`; risky ones ask first, and a hosts reset leaves a timestamped
  `.bak` beside the original.
- **Elevation on demand.** The manifest is `asInvoker`, so the whole report reads as a standard
  user; a repair that needs administrator rights offers to relaunch the same executable elevated.
- **Copy report.** A plain-text summary of every finding and repair goes to the clipboard.
- Modules: `ui` (custom GDI+ interface), `doctor` (checks, scoring, repair catalogue), `net`
  (ICMP, DNS, WinHTTP, WLAN, IP Helper, registry, process helpers), `main` (entry point).
  Roughly 5,190 lines.
- Disk defragmentation and TRIM are deliberately absent: they live on the suite's **Storage**
  page, not in a connection doctor.

---


## `bin\` — the shipped binaries

| File | Size | Imports |
|---|---|---|
| `BrowserCleaner.exe` | 1,288 KiB | `gdi32`, `gdiplus`, `kernel32`, `ole32`, `shell32`, `user32`, `api-ms-win-crt-*` |
| `DNSProtect.exe` | 1,348 KiB | `advapi32`, `comctl32`, `dwmapi`, `gdi32`, `iphlpapi`, `kernel32`, `ole32`, `shell32`, `user32`, `ws2_32`, `api-ms-win-crt-*` |
| `HardWin.exe` | 405 KiB | `advapi32`, `dwmapi`, `gdi32`, `kernel32`, `shell32`, `user32`, `api-ms-win-crt-*` |
| `PCSpeedTest.exe` | 601 KiB | `advapi32`, `dwmapi`, `gdi32`, `iphlpapi`, `kernel32`, `ole32`, `shell32`, `user32`, `winhttp`, `ws2_32`, `api-ms-win-crt-*` |
| `HomePolicy.exe` | 417 KiB | `advapi32`, `comdlg32`, `dwmapi`, `gdi32`, `kernel32`, `shell32`, `user32`, `api-ms-win-crt-*` |
| `InternetTuningWizard.exe` | 462 KiB | `advapi32`, `dwmapi`, `gdi32`, `iphlpapi`, `kernel32`, `shell32`, `user32`, `ws2_32`, `api-ms-win-crt-*` |
| `InternetConnectionUtility.exe` | 1,498 KiB | `advapi32`, `dwmapi`, `gdi32`, `gdiplus`, `iphlpapi`, `kernel32`, `shell32`, `user32`, `winhttp`, `wlanapi`, `ws2_32`, `api-ms-win-crt-*` |
| `SmartRAM.exe` | 396 KiB | `advapi32`, `d2d1`, `dwrite`, `gdi32`, `kernel32`, `ole32`, `shell32`, `user32`, `api-ms-win-crt-*` |
| `SystemTurbo.exe` | 1,338 KiB | `advapi32`, `comctl32`, `dwmapi`, `gdi32`, `kernel32`, `ole32`, `powrprof`, `shell32`, `user32`, `api-ms-win-crt-*` |

All of those are operating-system components. The `api-ms-win-crt-*`
entries are the Universal CRT, which ships with Windows 10 and 11; nothing
needs to be redistributed, and no Visual C++ runtime is involved. Binaries
are stripped (`-s`), so expect no debug symbols.

`bin\` is disposable output: `build_all.bat /clean` deletes and recreates it,
and `release.bat` packages it as-is. The tree keeps no build intermediates:
every program is compiled from its `src\` by `build_all.bat`, and the
per-project build trees, object files and single-program executables that
earlier builds left behind are not part of the repository.

---

## Adding future software

The suite is designed to grow: drop a new program folder next to the existing
ones and the build picks it up.

### Drop-in contract

```
NewToolName\
├── src\*.cpp                     required — every source file is compiled
├── src\ | res\ | resources\      optional — one *.rc (icon, version info,
│                                 optionally the manifest as `1 24 "app.manifest"`)
└── res\ | resources\ | src\      optional — app.manifest, embedded
    app.manifest                  automatically
```

Rules the script relies on:

- Only **top-level** folders containing `src\*.cpp` are treated as programs;
  `tools\`, sub-libraries and docs folders are ignored.
- The entry point must be `wWinMain` — every program is linked with
  `-municode -mwindows`.
- The resource script may reference its `.ico` / `.manifest` by bare file
  name; the script compiles it with its own folder on the include path.
- A manifest is embedded exactly once: either the `.rc` carries
  `1 24 "app.manifest"`, or a standalone `app.manifest` is found and gets
  wrapped into a linker override object. If neither exists, the MinGW default
  (`asInvoker`) is used.
- Data files belong under `%ProgramData%\<Name>\` (machine-wide, elevated) or
  `%LOCALAPPDATA%\<Name>\` (per user), not next to the executable, so `bin\`
  stays read-only-safe.

### Two ways to register it

1. **Explicit recipe (preferred).** Add one row to the `PROJECTS` table in
   `build_all.bat`:

   ```bat
   folder|OutputName|-luser32 -lgdi32 -lole32 ...
   ```

   That fixes the output file name and the exact link libraries.
2. **Automatic (fallback).** Do nothing. The folder is built with the generic
   library kit, the output name is the folder name with a trailing `-Modern`
   or `-Native` stripped, and the run flags it under
   *"built with the generic recipe (check link libs)"* so the link line can be
   tightened afterwards.

Either way the program is compiled statically, verified for foreign DLL
dependencies, and copied into `bin\` next to the others.

### Reserved slots

| Folder | Output | Status |
|---|---|---|
| `BrowserCleaner\` | `BrowserCleaner.exe` | present |
| `DNSProtect\` | `DNSProtect.exe` | present |
| `SmartRAM\` | `SmartRAM.exe` | present |
| `SystemTurbo\` | `SystemTurbo.exe` | present |
| `HomePolicy\` | `HomePolicy.exe` | present |
| `InternetTuningWizard\` | `InternetTuningWizard.exe` | present |
| `InternetConnectionUtility\` | `InternetConnectionUtility.exe` | present |
| *(next tool)* | *(from recipe table)* | add a folder plus, optionally, a recipe row |

When you add a program, also update the tables at the top of this file, the
per-program section below, and `CHANGES.md`.

Release packaging needs no update at all: `release.bat` packages every
`*.exe` in `bin\` and reads each one's name, version and description from
its version resource, so a newly built program shows up in the next ZIP and
in `MANIFEST.txt` automatically.

---

## Toolchain notes & troubleshooting

- **`-static` is mandatory.** Without it, a POSIX-threads MinGW build pulls in
  `libwinpthread-1.dll`; the suite script passes `-static` and then proves
  the result with `objdump`.
- **Apostrophes break `windres`.** The folder name `Zanphar's WinSuite`
  contains one, and `windres` mangles absolute paths that contain it. The
  suite script therefore copies each project's resource files into the
  staging folder and compiles them from there.
- **Spaces break the `-B` manifest override.** `g++ -B` with a path
  containing a space is split into two arguments by the linker driver. That
  is why the staging area must be space-free, and why the per-project
  `build_mingw.bat` scripts fail in this location.
- **`shlobj_core.h` is an MSVC SDK header.** MinGW ships everything in
  `shlobj.h`. The script generates a one-line compatibility header in the
  staging area (`#include <shlobj.h>` + `<knownfolders.h>`) and adds it to the
  include path, so no project source needs to change.
- **One manifest only.** MinGW's linker injects its own manifest unless a
  `default-manifest.o` is found earlier in the `-B` search prefix; getting
  this wrong yields `.rsrc merge failure: multiple non-default manifests`.
- **`-DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00`** is set for every project so
  Windows 10 APIs (`GetDpiForSystem`, per-monitor-v2 context) are declared.

---

## Conventions

- C++17, Win32 only, no third-party libraries, no COM registration.
- Static runtime, stripped release binaries, `wWinMain`.
- Dark hand-drawn UI (GDI+ or Direct2D/DirectWrite), per-monitor-v2 DPI
  aware, tray integration, single instance.
- All state in an INI or log file under `%ProgramData%` / `%LOCALAPPDATA%`;
  nothing in the registry except explicit opt-ins such as *Start with
  Windows*.
- `.clang-format` in each project defines the house style.

---

## License

BSD 3-Clause — see [LICENSE.md](LICENSE.md), copyright 2026-2027 Charles McDonald.
The suite's Settings page shows the same agreement read-only.

That one licence covers the whole product, the ported programs included:
`BrowserCleaner`, `DNSProtect`, `SmartRAM`, `SystemTurbo`, `InternetConnectionUtility` and the suite
window itself are all under the same terms, and `DNSProtect\LICENSE.md` and
`SystemTurbo\LICENSE.md` repeat them for those two programs. No part of this
tree is offered under MIT or any other licence.

These programs modify system state (browser profiles, DNS settings, power
plans, service start types, memory working sets). They are provided as-is,
without warranty; review the source before running them on a machine you
care about. Nothing here is affiliated with IObit or any other vendor.
