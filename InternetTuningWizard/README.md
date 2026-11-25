# Internet Tuning Wizard

A three-step network tuning wizard for Windows 10 and Windows 11: choose a
connection, flip the switches you actually want, review, apply.

It writes six settings Windows itself reads, and **Restore Windows defaults**
removes every one of them again. There is no service, no `netsh`, no scheduled
task and no resident process.

```
InternetTuningWizard.exe                  open the wizard
InternetTuningWizard.exe --list           every adapter and what this PC reads
InternetTuningWizard.exe --selftest       prove the model and the registry engine
InternetTuningWizard.exe --help           the same list on the console
```

## What it changes

| Setting | Registry value | Windows default | Applies |
|---|---|---|---|
| Network throttling off | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\NetworkThrottlingIndex = 0xFFFFFFFF` | throttling on (10 packets/ms) | next media-heavy session |
| IPv6 off | `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip6\Parameters\DisabledComponents = 0xFF` | IPv6 on (value absent) | restart |
| Multimedia CPU reserve off | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\SystemResponsiveness = 0` | 20 (a fifth of the CPU kept for multimedia) | next audio/video session |
| Closed connections freed sooner | `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\TcpTimedWaitDelay = 30` | Windows default (two minutes on many builds) | new connections |
| Nagle and delayed ACK off | `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}\TcpAckFrequency = 1`, `TCPNoDelay = 1`, `TcpDelAckTicks = 0` | Windows defaults (values absent) | next time that adapter connects |
| DNS resolver | same interface key, `NameServer = "1.1.1.1,1.0.0.1"` and `DhcpNameServer = "1.1.1.1,1.0.0.1"` | whatever DHCP hands out | next time that adapter connects |

`{GUID}` is the adapter the wizard's first step selects. The other three rows are
machine-wide.

The resolver choices are **System default**, **Cloudflare** (`1.1.1.1`,
`1.0.0.1`), **Google** (`8.8.8.8`, `8.8.4.4`) and **Quad9** (`9.9.9.9`,
`149.112.112.112`).

On a DHCP adapter Windows keeps the manual list in `NameServer` and the lease's
list in `DhcpNameServer`, and uses `NameServer` while it is present. Measured on
Windows 11 26300: after `ipconfig /renew` the wizard's `NameServer` was still
`1.1.1.1,1.0.0.1`, while Windows rewrote `DhcpNameServer` back to the router's
`192.168.4.1`. So the wizard writes the chosen servers to **both** values - the
static one is what sticks, and the DHCP side carries the same list for a
reconnect that falls back to it - and saves the adapter's previous pair before
the first apply. **Restore Windows defaults** puts the saved pair back exactly,
and deletes the saved copy; with nothing saved it removes the wizard's values so
Windows learns the DNS again from the network.

## What each row really does

- **Network throttling** is the Multimedia Class Scheduler cap: while audio or
  video is playing, Windows limits network traffic to roughly 10 packets per
  millisecond. Removing the cap helps games and streaming on a busy line; it
  does nothing on an idle connection and does not raise your bandwidth.
- **IPv6 off** is for networks whose IPv6 is advertised but broken, which makes
  name lookups and connections stall until they fall back to IPv4. A restart
  finishes it, and some VPNs, Teredo and DirectAccess need IPv6 - leave it on
  if yours does.
- **Nagle and delayed ACK** are TCP batching rules that trade latency for
  packet efficiency. Turning them off (including the 200 ms delayed-ACK delay)
  helps small, frequent writes (games, remote desktop, trading tools); it can
  add a little overhead on bulk transfers.
- **Multimedia CPU reserve** is the same scheduler family as network
  throttling: Windows keeps a percentage of the CPU for audio and video, 20 by
  default, and `SystemResponsiveness = 0` hands that share back to games and
  foreground work. It is a CPU share, not bandwidth, and audio can stutter on a
  PC that is already at 100%.
- **TIME_WAIT** is how long a closed connection keeps its port reserved. The
  default is two minutes on many builds; 30 seconds frees ports sooner for a
  client that opens many short connections. It does not make connections
  faster, and Microsoft only recommends it for specific high-connection
  workloads.
- **DNS** chooses who answers name lookups for that adapter. A faster resolver
  shortens the wait before a connection starts; it does not change the speed of
  the connection itself. The cache is flushed after every write.

The wizard does **not** promise a faster internet connection. It writes the
values; the network and Windows decide the rest.

## Using it

1. **Connection.** Pick one of the connected adapters — Ethernet, Wi-Fi, VPN,
   Hyper-V switch, anything with a driver — and the list scrolls (wheel, arrow
   keys, dragging the scrollbar's thumb, or clicking its track) when there are
   more adapters than fit, so none can hide below the fold. Its driver, IPv4
   address and current resolvers are shown, and the DNS row applies to that
   adapter only. **Next**.
2. **Tuning.** Five switches, all off by default: network throttling off, IPv6
   off, the multimedia CPU reserve off, TIME_WAIT capped at 30 seconds, and
   Nagle and delayed ACK off for the connection chosen in step 1 - that switch
   names the adapter it will tune. A switch left off is neither written nor
   removed - **Restore Windows defaults** is the reset. **Next**.
3. **Review.** The page lists the connection, the DNS choice and every value
   Apply will write or remove; the automatic DNS choice is the one removal, as
   it clears the adapter's override. **Apply changes**. The result page then
   shows one green or red row per value, with **Back** to the review,
   **Restore Windows defaults** to undo the run on the spot, and **Close**.

**Apply changes** asks for administrator rights once and writes only the
switches that are on (plus the DNS choice). The unelevated window hands the plan
to an elevated copy of itself on the command line; that copy writes, reads the
values back and reports through a small text file in `%TEMP%`, which the window
turns into a green or red row per value. Declining the prompt
changes nothing: the result page says so in a red row and **Back** returns to
the review.

**Restore Windows defaults** needs two clicks (the second confirms): it removes
`NetworkThrottlingIndex`, `DisabledComponents`, both TCP values, the static
`NameServer` and the adapter's `DhcpNameServer` (put back as it was, not just
deleted), then flushes DNS. A value another tool set is removed too - the wizard
cannot tell who wrote it, which is why the button confirms.

## Console mode

- `--list` prints each adapter with its resolvers, whether Nagle is tuned, and
  what the four machine-wide values currently read. It needs no elevation.
- `--selftest` prints PASS/FAIL for seven checks and exits non-zero on a
  failure: the provider table, the plan text used for the elevated handoff, the
  review text, a registry write/read/remove round trip on
  `HKCU\Software\ZanpharsWinSuite\InternetTuningWizard\selftest` (which it
  deletes), the adapter enumeration and the documented constants.

## Limits, stated plainly

- **Six settings only.** Anything else that calls itself "internet tuning" -
  `MaxUserPort`, "reserve QoS bandwidth", the `Tcp1323Opts` / `TcpWindowSize`
  experiments - is either deprecated on Windows 10/11, a myth, or a good way to
  break throughput, and is deliberately not here. TIME_WAIT is the one
  situational value: it helps clients that exhaust ports, nothing else.
- **Machine-wide means machine-wide.** Throttling, IPv6, the multimedia reserve
  and TIME_WAIT affect every account and every adapter; only DNS and Nagle are
  scoped to the chosen one.
- **A reconnect, not a reboot, for DNS and Nagle.** Windows reads the interface
  values when the adapter connects. `Restart as needed` - IPv6 is the only row
  that asks for a reboot.
- **One write path.** `RegCreateKeyEx` / `RegSetValueEx` / `RegDeleteValue`
  under the 64-bit view, plus `DnsFlushResolverCache` from `dnsapi.dll`.
- **A protected key can still refuse.** If Windows rejects a write, the report
  says so in red and nothing is rolled back silently; Restore is one button
  away.

## Icon

A speedometer: accent arc, bright needle, accent hub, on the suite's dark card.
At 16 and 24 px the arc and needle thicken and the tick dots are dropped, so the
mark stays legible in the title bar and on the taskbar.

```bat
powershell -ExecutionPolicy Bypass -File InternetTuningWizard\tools\make_icon.ps1
```

## Building

```bat
InternetTuningWizard\build_mingw.bat      this program only
build_all.bat                             the whole suite
```

Both link statically with `-municode -mwindows`; the only DLLs the output uses
are Windows' own (`advapi32`, `dnsapi`, `dwmapi`, `gdi32`, `iphlpapi`,
`kernel32`, `shell32`, `user32`, `ws2_32` and the Universal CRT).
