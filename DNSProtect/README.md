# DNS Protect

A small Windows utility that protects system DNS settings from unauthorized
changes, the way DNS-hijacking malware changes them. Written in C++17 with
plain Win32 — no third-party dependencies.

Inspired by the "DNS Protection" helper that ships with Advanced SystemCare.
This is an independent, from-scratch implementation: no code is shared with
the original.

## What it does

- **Enforces a trusted DNS configuration** on every IP-enabled network
  adapter: your current system settings, a public provider
  (Google, Cloudflare, OpenDNS, Quad9, Comodo, Verizon, DNS Advantage),
  or a custom preferred/alternate pair.
- **Reverts hijacks automatically.** A background engine watches the
  per-adapter DNS registry values (`NameServer`, `DhcpNameServer`) and snaps
  unauthorized changes back to the approved configuration, typically within
  1.5 seconds. Every incident is logged.
- **One window, ready to run.** A dark, DPI-aware dashboard shows protection
  status at a glance, plus a live adapter list. Minimizes to the tray.

## Project layout

```
├── src/
│   ├── app.cpp        process entry: single instance, elevation, wiring
│   ├── ui.cpp/.h      custom-drawn dashboard, tray integration
│   ├── dns.cpp/.h     protection engine (watchdog, apply/restore, baseline)
│   ├── config.cpp/.h  provider catalogue + persisted settings (dnsprotect.ini)
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

`DNSProtect.exe` needs administrator rights (it writes to
`HKLM\SYSTEM\...`), so it self-relaunches with a UAC prompt. If the prompt
is declined it runs in read-only mode.

Typical flow: pick a provider, press **Apply & protect**, close to the tray.
To verify, change your DNS in Windows Settings or with
`netsh interface ip set dns ...` — the change is reverted almost instantly,
counted in the status card, and written to
`%ProgramData%\DNSProtect\dnsprotect.log`.

## Scope and limitations

- Operates on the classic IPv4 stack (`Services\Tcpip\...`), like the
  tool that inspired it. IPv6-only resolver settings and DNS set via Group
  Policy / MDM are not monitored.
- Protection is snapshot-and-restore, not kernel-level write blocking:
  a rogue writer briefly sees its value accepted before it is reverted.
- Static-DNS changes are nudged into effect via `DhcpNotifyConfigChange`
  plus `ipconfig /flushdns`; some systems may still need an adapter
  disable/enable for an immediate effect.

## License

BSD 3-Clause, copyright 2026 Charles McDonald — see `LICENSE.md`.
