# Pulse

A lightweight, always-on Windows widget that shows live system metrics — CPU, RAM, GPU, network, disk, top processes — without the bloat of Task Manager.

![widget screenshot](docs/screenshots/widget.png)

## Why

Task Manager works but it's too big, doesn't remember its size/position, and you have to launch it manually every time. Pulse aims to be a small, always-on widget that **respects your system's resources** — a monitoring tool that consumes a fraction of a percent of CPU and ~190 MB RAM has no business existing if it doesn't actually stay lean.

Pulse:

- Starts automatically with Windows (optional)
- Remembers position and size between sessions
- Lives in the system tray (one click to hide/show)
- Shows live sparklines for the last 60 seconds with smooth Bezier curves
- Stays under **2% CPU of a single core** in steady state (~0.05% of total system on modern CPUs)

## Lightweight by design

Pulse was built with obsessive focus on minimising its own footprint. After several profiling iterations the current numbers on the reference setup (**AMD Ryzen 9 7950X, 32 threads, NVIDIA RTX 5070 Ti, 64 GB DDR5, Windows 11**) are:

| Metric | Value | What it means |
|---|---|---|
| **Cold-start time** | ~540 ms | Time from launch to message loop ready (single-file extract included) |
| **Working Set (RAM)** | ~194 MB | What Task Manager shows; mostly shared with other .NET apps |
| **Private memory** | ~144 MB | RAM uniquely owned by Pulse |
| **CPU avg (incl. startup)** | ~4% | 120s window — startup spike dominates |
| **CPU steady-state (≥30 s)** | **~1.5%** of one core | ≈ 0.05% of total system on a 32-thread CPU |
| **CPU max spike** | ~12% of one core | During first slow-tick init |
| **Handles** | ~850 | Stable, no leak |
| **Threads** | ~20 | Stable |
| **RAM growth over 2 min** | ~0 MB | No leaks measured |
| **Single-file exe** | 7 MB (10.3 MB in v1.2.0, 10.5 MB in v1.3.0) | + 1.5 MB of native LHM dependencies; v1.2.0 embeds the Reticle UI library and its fonts |
| **Installer** | 4.4 MB | + portable zip 3.7 MB |

The table was measured on v1.1.0. For v1.2.0 and v1.3.0 see below.

### v1.2.0: the Reticle UI

v1.2.0 moves the widget to Reticle, the author's WPF design system, and drops the old layered transparent window (`AllowsTransparency`) with a blurred drop shadow: every update redrew the shadow over the whole window and copied the window back from the GPU. The window is now a regular one with the Windows 11 Mica backdrop. Measured with the PC idle, same 622 × 502 window, v1.1.0 against v1.2.0 alternated in two 45 s rounds (not elevated, so the figures are not comparable with the table above, only with each other):

| Metric | v1.1.0 | v1.2.0 |
|---|---|---|
| CPU, widget visible | 5.8% of one core | **4.9%** |
| CPU, widget hidden in the tray | 4.8% | **4.4%** |
| GPU (3D engine) | 0.21% | **0.09%** |
| Dedicated video memory | 34 MB | **17 MB** |
| Private memory / working set | 145 / 200 MB | 130 / 211 MB |
| Cold start (message loop ready) | ~506 ms | ~540 ms |

### v1.3.0: lighter sensors, Reticle 0.6

v1.3.0 reads NVIDIA GPUs directly with NVML instead of LibreHardwareMonitor's 41 sensors (a sensor read went from 84 to 4.8 ms), reads only the GPU it shows, starts `netsh` only on Wi-Fi, and moves to Reticle 0.6, whose window no longer uses WPF's `WindowChrome`. Measured on 11/10/2026 with the PC idle, same 622 × 502 window, v1.2.0, the v1.2.0 code on Reticle 0.6 alone, and v1.3.0 alternated in two 45 s rounds (not elevated; v1.2.0 measured again in this session, so compare within this table only):

| Metric | v1.2.0 | Reticle 0.6 alone | v1.3.0 |
|---|---|---|---|
| CPU, widget visible | 2.50% of one core | 2.72% | **2.10%** |
| CPU, widget hidden in the tray | 2.65% | 2.55% | **1.53%** |
| Compositor (dwm) CPU, widget visible | 6.6% | 4.1% | **2.5%** |
| Compositor (dwm) GPU, widget visible | 1.67% | 0.60% | **0.57%** |
| GPU (3D engine) of Pulse | 0.05% | 0.03% | 0.04% |
| Private memory / working set | 130 / 211 MB | 130 / 209 MB | 131 / 214 MB |
| Cold start (message loop ready) | ~590 ms | ~477 ms | ~510 ms |

Hidden in the tray the widget draws nothing, so that row is the data collection alone: about one point of a core less, what NVML saves every two seconds. With the widget visible part of it goes to the new core and memory bars. The compositor rows are the noisiest (other windows on screen move them too), but they went down in both rounds: the window without `WindowChrome` costs Windows less.

### v1.3.1: a PC without a graphics card

v1.3.1 was checked on a second PC, the opposite of the reference one: Intel Core i9-11900K with only its integrated UHD Graphics 750, Windows 11 in Italian, Hyper-V, WSL and a VPN (64 network interfaces). There v1.3.0 showed the GPU at 0% and the CPU at 3.50 GHz at all times; v1.3.1 shows the integrated GPU and the real CPU speed, and reads the network with less work on the UI thread. Measured on 12/10/2026 with the PC idle, same 436 × 551 window, v1.3.0 and v1.3.1 alternated in two 45 s rounds (not elevated; another PC, so compare within this table only):

| Metric | v1.3.0 | v1.3.1 |
|---|---|---|
| CPU, widget visible | 3.13% of one core | **2.87%** (within the noise) |
| 1 s tick on the UI thread | ~4 ms, peaks over 100 ms | **~1 ms** |
| Private memory | ~100 MB | ~100 MB |
| GPU | 0%: the integrated GPU was not read | the busiest D3D engine, as in Task Manager |
| CPU clock | 3.50 GHz, the base clock, at all times | 2.5–3.0 GHz at rest, 4.6 GHz under load |

As administrator, the way Pulse runs on that PC, two more alternated rounds gave 4.00% and 3.70% of one core: the PC was busier, and the gap is within the noise there too.

### How we got here (the things that matter)

1. **No PerformanceCounter** — every metric is read via direct Win32 P/Invoke (`GetSystemTimes`, `GetPerformanceInfo`, `GetIfTable2`, `NtQuerySystemInformation`, `CallNtPowerInformation`). Each tick is ~5 ms instead of the ~45 ms it would cost using `PerformanceCounter`/PDH.
2. **No `System.Windows.Forms`, no `System.Drawing`** — the tray icon is a hand-rolled Win32 `Shell_NotifyIconW` wrapper, the icon is loaded via `ExtractIconEx` from the exe itself. This drops 20–30 MB of assemblies that WPF apps usually pull in.
3. **No `System.Management`/WMI** — we replaced the only two WMI queries (CPU temp fallback, VRAM total) with cheaper alternatives.
4. **Pooled buffers in the sparkline** — the `Point[]` array and gradient brushes are reused across renders instead of being allocated every frame.
5. **Lazy initialisation of LibreHardwareMonitor** — sensor enumeration happens in the background after the UI is already on screen.
6. **Staggered polling** — fast metrics (CPU/RAM/Net/Disk) tick at 1 s, hardware sensors at 2 s, top processes at 5 s, drive list at 30 s. No one tick does too much.
7. **ReadyToRun precompile** — managed IL is precompiled to native code at publish time (`-p:PublishReadyToRun=true`), shaving ~30% off the cold-start JIT cost.

A full performance log lives in `%TEMP%\pulse-perf.log`: any tick that exceeds 50–500 ms (depending on the operation) gets recorded so we can spot regressions.

## Features

- **CPU**: usage %, current clock (GHz: the fastest core's with the MSR sensors, otherwise the average speed of the cores at work, as in Task Manager), temperature, power (W) — see "Running as administrator" below; under the graph one bar per logical processor (threads of the same core side by side) and the load counted in full cores, to see how parallel the work is; as administrator the bars also show, in colour, how much of each core the busiest processes of *Top CPU* take (up to three, from half a core up)
- **RAM**: usage %, used/total GB, available GB and, under the graph, the composition bar of Task Manager (in use, modified, cached, free); the tooltip explains each part and adds committed memory and page file usage
- **GPU**: usage %, VRAM used/total, temperature, core/memory clocks, power (W); on a PC without a graphics card the integrated GPU, with the load of its busiest engine (as in Task Manager) and the memory it shares with the CPU
- **Network**: ↓/↑ throughput, Wi-Fi SSID/signal, local IP, public IP, ping to 8.8.8.8
- **Disk**: total ↓/↑ throughput, per-drive free space + bar, SSD temp (when LHM Storage is enabled)
- **Top processes**: top 6 by CPU (those above half a core with their colour in the core bars, the load in cores in the tooltip) and by RAM, updated every 5 s. Right-click on a process for: *Open file location* · *Properties* · *End process* · *Search online*
- **System**: uptime, estimated total PC power draw (W)
- **Fit button** in the header: the widget takes the smallest size that shows everything, with no scroll bar and no value cut, even at its longest
- **Corner button** next to the pin: a click opens four arrows, and the chosen one sends the widget to that corner of its screen, 12 units from the edges and never over the taskbar
- **Themed** with the Reticle UI (from v1.2.0): dark, light or follow Windows from the tray menu; accent colour, Mica transparency, fill and density from *Appearance...*; smooth Bezier sparklines on a dotted grid, tabular digits so live values never shift the row, custom drag/resize, always-on-top toggle

## Running as administrator (important)

**This is the single most impactful setting.** Several CPU sensors live in Model-Specific Registers (MSRs) which only kernel mode can read, through a driver: the PawnIO driver, installed once (see [below](#why-admin-alone-isnt-enough-pawnio)). Without admin privileges and PawnIO Pulse cannot show:

- CPU temperature (per-core and package)
- CPU package power (W)
- The clock of the fastest core (Pulse shows the average speed of the cores at work instead, as Task Manager does)

And without admin privileges alone:

- Which process runs on which core (the colours in the core bars): the kernel sampling session needs elevation, not PawnIO

Everything else (CPU/RAM/GPU usage, GPU temp/power, network, disk, top processes) works fine without elevation — these don't need MSR access.

### The clean way: tick "Start with administrator privileges" in the installer

When you run `Setup-Pulse-vX.Y.Z.exe`, the wizard offers two startup options:

- ☐ **Start with Windows** — registers `HKCU\…\Run` so Pulse launches on login (regular user privileges, no UAC prompt)
- ☐ **Start with administrator privileges** ← *check this if you want CPU temp/power*

The second checkbox creates a **Windows Scheduled Task** with trigger *On Logon* and *Run with highest privileges*. This is the standard workaround used by HWiNFO64, ThrottleStop, MSI Afterburner et al. to avoid the UAC prompt at every login. Once configured, Windows itself launches Pulse with an elevated token at boot — no clicking, no warnings. The task has no run-time limit (the Task Scheduler default stopped Pulse after 72 hours) and starts on battery too.

With *Install for me only*, the installer's default mode, Setup itself has no administrator rights, and Windows lets only an administrator create a task with the highest privileges: ticking the option then brings up one UAC prompt while installing (and one when uninstalling, to remove the task). *Install for all users* asks for it once at the start instead. Unticking the option in a later update removes the task, and Pulse goes back to starting without the rights.

Pulse uses the same task when it is started without the rights: at the end of the installer (Windows runs *Launch Pulse* as a normal user), from the Start menu or from the desktop icon it starts the task and closes, and it is back elevated in about a second, again with no UAC prompt. So after installing or updating it has the administrator rights at once, not from the next logon.

A third checkbox, ticked by default, **restarts Pulse if it closes because of an error**, with the same rights it normally starts with: a second Scheduled Task, *Pulse restart*, that fires on the crash event Windows writes for `Pulse.exe` (see [`docs/INSTALLER.md`](docs/INSTALLER.md)). The case it was made for: an NVIDIA driver update crashed an elevated Pulse, which stayed closed until it was reopened by hand, without the administrator rights.

The Scheduled Tasks are removed when you uninstall Pulse.

### Why admin alone isn't enough: PawnIO

Reading MSRs takes a kernel driver, and LibreHardwareMonitor, the library Pulse reads the CPU sensors with, no longer brings one of its own: version 0.9.6 uses only **PawnIO**, a signed driver installed separately, once, from [pawnio.eu](https://pawnio.eu) or with:

```powershell
winget install namazso.PawnIO
```

Without it the CPU temperature reads `n/a` and the CPU power is missing even with Pulse running as administrator, on Intel and AMD alike. The tooltip on `n/a` says what is missing: the driver, the rights or both.

See [`docs/HARDWARE-SUPPORT.md`](docs/HARDWARE-SUPPORT.md) for the full breakdown per CPU family.

## Install

### Option A — Installer (recommended)

Download the latest `Setup-Pulse-vX.Y.Z.exe` from the [Releases page](../../releases). The installer offers:

- ☐ Start with Windows (login)
- ☐ **Start with administrator privileges** (see section above — with the PawnIO driver, enables CPU temp/power)
- ☑ Restart Pulse if it closes because of an error (ticked by default)
- ☐ Create desktop shortcut

Inno Setup detects an existing Pulse install via its `AppId` and **upgrades in place without losing settings** (settings live in `%APPDATA%\Pulse\settings.json` and are preserved across uninstall/reinstall).

### Option B — Portable build

Grab `Pulse-vX.Y.Z-portable.zip` from Releases, extract anywhere, and run `Pulse.exe`. Settings are stored in `%APPDATA%\Pulse\settings.json`. To enable CPU temp/power in portable mode you have to install PawnIO and launch the exe manually with *Run as administrator* every time.

## Hardware requirements

- Windows 10 / 11 (x64); the Mica and Acrylic transparency needs Windows 11 22H2 or later (elsewhere the widget is opaque)
- .NET 9 Desktop Runtime ([download](https://dotnet.microsoft.com/download/dotnet/9.0))

Optional, to unlock CPU temperature / power (Intel and AMD): the [PawnIO](https://pawnio.eu) driver (`winget install namazso.PawnIO`) and Pulse running as administrator, best through the installer's *Start with administrator privileges*. Without them CPU temp/power show as `n/a` and the clock is the average speed of the cores; everything else still works.

## Tray menu

Right-click the tray icon (near the clock) for:

- Show / Hide — toggle widget visibility
- Theme — Dark / Light / Follow Windows
- Appearance... — accent colour, transparency, fill and density, applied live
- Always on top — pin toggle
- Start with Windows — autostart (regular user privileges)
- Restart as administrator — re-launch elevated, enables the process colours in the core bars and, with PawnIO installed, the MSR sensors (only shown when not already admin)
- Exit

## SmartScreen warning when downloading

Pulse is **not currently code-signed** (an EV certificate costs ~$300/year and we'd rather spend that on something else for now). The first time you download `Setup-Pulse-vX.Y.Z.exe`, Windows SmartScreen may show *"Windows protected your PC"* with a *"Don't run"* button.

Click **"More info"** → **"Run anyway"**. This is the same flow Notepad++, HWiNFO and many other open-source tools require until they earn enough reputation or pay for an EV cert.

## Build from source

See [`docs/BUILD.md`](docs/BUILD.md) for full instructions. TL;DR:

```bash
git clone https://github.com/caste9612/Pulse
cd Pulse/ResourceMonitor
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o ../dist
```

Output: `dist/Pulse.exe` (~10 MB, with ReadyToRun precompiled). The Reticle UI library ships as DLLs in `lib/Reticle`, so the clone builds as is.

## Architecture

WPF UI on top of background services polling at staggered intervals. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

```
MainWindow (WPF + Reticle UI, Mica backdrop)
   │
   ├── MetricsService    (1 s : CPU/RAM/Disk/Net via Win32 native)
   ├── NativeMetrics     (P/Invoke: GetSystemTimes, GetIfTable2, ...)
   ├── HardwareMonitor   (2 s : LibreHardwareMonitorLib — temps/clocks/watts)
   ├── CpuSpeed          (2 s : average CPU speed via PerfLib V2, when there are no MSR clocks)
   ├── ProcessMonitor    (5 s : top processes via Process.GetProcesses)
   ├── NetworkMonitor    (2 s : ping, IPs, SSID via netsh)
   ├── DriveMonitor      (30 s: DriveInfo.GetDrives)
   └── TrayIcon          (Win32 Shell_NotifyIconW, no WinForms)
```

## License

MIT. See [LICENSE](LICENSE).

Bundles LibreHardwareMonitorLib (MPL 2.0). No other third-party runtime dependencies.
