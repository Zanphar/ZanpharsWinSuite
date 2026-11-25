# System Turbo

A small Windows utility that applies reversible performance tweaks (the
"Turbo Boost" half) and keeps live RAM/CPU instrumentation with a
one-click or automatic memory-release pass (the "Accelerate" half).
Written in C++17 with plain Win32 — no third-party dependencies.

Inspired by the Turbo Boost and Hardware Accelerate modules that ship with
Advanced SystemCare. This is an independent, from-scratch implementation:
no code is shared with the original.

## What it does

- **Turbo boost, fully reversible.** Switches the active power plan to High
  Performance, parks a small set of safe background services (Windows
  Search, SysMain, Print Spooler, Bluetooth Support, Diagnostic Policy,
  Fax, WMP Network Sharing) in manual-start mode while they are stopped,
  and disables Game DVR background recording. Every original value is
  captured into persisted restore records, so the boost survives a
  restart and is undone exactly on power-off.
- **Live memory & CPU card.** RAM load, free bytes and CPU load update
  in place, alongside the heaviest images (summed per executable),
  colour-coded by their share of total RAM.
- **Memory release.** One click (or the tray menu) empties large working
  sets via `EmptyWorkingSet` and purges the standby list through
  `NtSetSystemInformation`. 'Free RAM when boosting' runs the same pass
  as the boost kicks in, and 'Automatically free memory' fires it
  whenever RAM load reaches 80%.
- **One window, ready to run.** A dark, DPI-aware dashboard that can be
  resized or maximized: cards and buttons follow the client area, and it never
  shrinks below its 660 x 752 design size, so labels and hit targets stay
  intact. Minimizes to the tray; single-instance; self-relaunches with a UAC
  prompt (declining keeps instrumentation alive, boost tweaks are rejected).

## Project layout

```
├── src/
│   ├── app.cpp        process entry: single instance, elevation, wiring
│   ├── ui.cpp/.h      custom-drawn dashboard, tray integration
│   ├── turbo.cpp/.h   boost engine (tweaks, restore records, worker thread)
│   ├── ram.cpp/.h     memory probes, top-process list, release passes
│   ├── config.cpp/.h  persisted options + boost bookkeeping (systemturbo.ini)
│   ├── theme.cpp/.h   colour palette, fonts, DPI scaling
│   ├── log.cpp/.h     thread-safe application log
│   └── common.h       shared types and utilities
├── resources/         manifest, version info, generated application icon
├── tools/make_icon.ps1  reproducible icon build pipeline (GDI+)
├── build_mingw.bat    one-command build for the MinGW-w64 toolchain
└── CMakeLists.txt     CMake build (MinGW or MSVC)
```

## Building

### MinGW-w64 (WinLibs)

Requires `windres.exe` and `g++.exe` on `PATH`; otherwise set `MINGW_BIN`
before running:

```bat
build_mingw.bat
```

### CMake (MinGW or MSVC)

```bat
cmake -S . -B build
cmake --build build --config Release
```

For MSVC the manifest goes through `/MANIFEST:EMBED`; for MinGW it is
embedded as a `-B`-search-prefix `default-manifest.o`, which *replaces* the
toolchain's stock manifest (MinGW's linker otherwise reports a duplicate
`.rsrc` manifest).

## Running

`SystemTurbo.exe` needs administrator rights (power plan, SCM and
`SeProfileSingleProcessPrivilege` all require elevation), so it
self-relaunches with a UAC prompt. If the prompt is declined it runs in
instrumentation-only mode.

Typical flow: tick the option groups, press **Turbo now**, close to the
tray. To verify, open Windows Settings > Power (the scheme switches to
High performance) or `sc query WSearch` (shows STOPPED). Press
**Turn off & restore**, and every captured value is written back —
including the original power plan GUID and manual-start services that
used to run.

Everything is recorded in `%ProgramData%\SystemTurbo\systemturbo.log`;
boost state and restore records live in `systemturbo.ini` next to it.

## Scope and limitations

- Parking a service in manual-start mode only bites once it is (re)started
  by something else while the boost is on; restoring re-applies the
  original start type and restarts services that were running before.
- The freed-memory figure compares free physical bytes before and after
  the pass: a convenient measure of the effect, not a bookkeeping of
  exactly which pages moved.
- The standby-list purge is best effort; on systems where the privilege
  cannot be enabled only working-set trimming happens.
- Game DVR values live under HKCU of the elevated user, which for a
  run-as-same-user elevation is the same hive you log in with.

## License

BSD 3-Clause, copyright 2026-2027 Charles McDonald — see `LICENSE.md`.
