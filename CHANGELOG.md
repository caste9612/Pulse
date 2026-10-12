# Changelog

All notable changes to Pulse will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v1.3.3] - 2026-10-12

### Added
- **PawnIO from the installer**: CPU temperature, power and core clocks need the PawnIO driver, which LibreHardwareMonitor 0.9.6 reads the CPU's registers through and doesn't ship; on a new PC they read n/a until someone installs it by hand. Under *Start with administrator privileges* the installer now offers **Install the PawnIO driver**, ticked with its parent (only an elevated Pulse can use the driver) and shown only when PawnIO is missing. It downloads the official setup of PawnIO 2.2.0 from its GitHub release when the user clicks *Install*, checks it against the SHA256 of winget's manifest (the file is signed by namazso), and runs it elevated with winget's silent switches before Pulse starts, so the temperature shows at once; with *Install for me only* that is one more UAC prompt. PawnIO isn't bundled, since its releases come with no licence to redistribute them. A failed download doesn't stop the installation: a message says how to install PawnIO later. Uninstalling Pulse leaves PawnIO, which other programs use too. Building the installer now needs Inno Setup 6.1, for its download page. Checked on the i9-11900K test PC with PawnIO uninstalled first: a silent install over v1.3.2 with both options downloaded PawnIO 2.2.0, registered the tasks and installed the driver in 13 s, and the Pulse started by the logon task showed the CPU temperature and power again.

## [v1.3.2] - 2026-10-12

### Changed
- **Temperatures and power on the line below**: the CPU's first line held the load, the clock, the power and the temperature, which pushed its graph to the right (from x 186 to 262 on a 436-wide widget), while the line under it had only "≈ 4.6 cores in use" and empty space under the clock, power and temperature. The temperature and the power now open that line ("87°C  132.0W  ≈ 4.6 cores in use"), next to the core bars, and the CPU graph starts where the RAM's does; the GPU temperature opens the GPU's detail line, before clocks and power. The name of an integrated GPU now always leads its detail line ("Intel UHD Graphics 750 • 1.9W"), which otherwise held only the power. The Fit size doesn't change: the network and disk lines set the width.

## [v1.3.1] - 2026-10-12

Checked on a second PC, the opposite of the reference one: Intel Core i9-11900K, no graphics card (Intel UHD Graphics 750), Windows 11 in Italian, Hyper-V, WSL and a VPN.

### Fixed
- **The GPU read 0% on a PC without a graphics card**, with no memory, clocks or name: Pulse looked for the "GPU Core" load and the "GPU Memory" sensors, which only NVIDIA and AMD cards have. An Intel integrated GPU has only the load of its D3D engines (3D, copy, video...) and memory shared with the CPU. The load is now that of the busiest engine, as in Task Manager (on the test PC 46.6% against the 47.1% of the *GPU Engine* counters Task Manager reads, and as close over eight seconds), the memory reads "shared 2.0 / 15.9 GB" with a tooltip, and the GPU name takes the detail line left empty by clocks and power. An integrated GPU is shown only when no card has memory of its own, and its power never goes into the PC estimate: it is already in the CPU package.
- **The CPU clock stuck at the base clock** without the MSR sensors (no PawnIO, see below, or no administrator rights): Windows' `CallNtPowerInformation` gives the base clock on recent Intel processors, so the widget read 3.50 GHz at rest and under load alike on the i9-11900K. It now shows the average speed of the cores at work, as Task Manager does: the base clock times the "% Processor Performance" of the kernel's *Processor Information* counters, read with the PerfLib V2 consumer API (advapi32). On the test PC 2.5–3.0 GHz at rest and 4.6 GHz with eight busy threads, the same values as PDH, which cost 6.4 MB of private memory and 420 ms at start against under 1 MB and about 0.4 ms per read. The tooltip says which clock is shown. With the MSR sensors the clock is the fastest core's, as before.
- **The CPU temperature tooltip** said to run Pulse as administrator even when it already was. It now says what is missing: the PawnIO driver, the rights, or both.
- **The Wi-Fi signal was missing on a Windows in another language**: it was read from `netsh wlan show interfaces` by its English label, *Signal*, which netsh translates (*Segnale* in Italian). Without the English label it is now the line that ends with a percentage, the only one in that list. Checked on netsh outputs in Italian and English; not on a live connection, the test PC's Wi-Fi being off.

### Changed
- **PawnIO**: LibreHardwareMonitor 0.9.6 reads the CPU temperature, power and core clocks only through the PawnIO driver, which it doesn't ship: it has to be installed (pawnio.eu, `winget install namazso.PawnIO`), and without it those sensors stay empty even as administrator, on Intel and AMD alike. README and HARDWARE-SUPPORT no longer send AMD users to Ryzen Master and Intel users to a driver of LibreHardwareMonitor's own: the library uses neither.
- **Network, lighter on PCs with many interfaces**: the throughput read the whole interface table (`GetIfTable2`) every second on the UI thread, 64 interfaces on the test PC (Hyper-V, WSL, a VPN, the filters of a camera driver): 2.4–3 ms per read, with peaks over 100 ms. The adapters to count are now chosen every 10 seconds, and at once when one stops being up, and only those are read every second (`GetIfEntry2`): 0.3 ms. The active adapter and the local IP (`GetAllNetworkInterfaces`, about 30 ms there) are read when Windows reports an address change and once a minute, instead of every 2 seconds.

### Performance (i9-11900K test PC, PC idle, same 436 × 551 window, v1.3.0 vs this build alternated in two 45 s rounds on 12/10/2026, not elevated)
- CPU: 3.13% → 2.87% of one core (3.58 and 2.67 against 2.64 and 3.09: within the noise; most of the 30 ms of the adapter scan was waiting, not CPU). As administrator, the way Pulse runs on that PC, the CI build of this version against v1.3.0 in two more alternated rounds: 4.00% → 3.70%, with the PC busier, within the noise too
- 1 s tick on the UI thread: about 4 → 1 ms, network peaks over 100 ms gone
- Private memory: about 100 MB in both. The working set of a newly built exe is about 80 MB higher in its first runs on this PC, the same for v1.3.0 built here: the Intel driver compiles WPF's shaders for a new executable, then reuses them. Compare working sets after a couple of runs.

## [v1.3.0] - 2026-10-11

### Added
- **Corner button** in the header, next to the pin (Reticle's four corner ticks): a click opens four arrows that slide out towards their corners (`motion.reveal`, no motion when Windows animations are off), and the chosen one sends the widget to that corner of the screen it is on. It stays inside the work area, so never over the taskbar, 12 units (Reticle space S3) from both edges, the same margin as the first-run position. Esc, a click elsewhere or leaving the widget closes the arrows.
- **Fit button** in the header, next to the corner button: the widget takes the smallest size that shows everything. The height is exactly the content (no scroll bar, no empty space at the bottom); the width is the narrowest where no value is cut, measured with each value at its longest text (999.9 MB/s, 99 days of uptime, 100 %), so a fit taken at low traffic or right after startup doesn't cut anything later, and with the graphs at their minimum width. A widget on the right or bottom edge of the screen stays there, and inside the work area. Checked from four starting sizes: always 446 × 557 on the reference PC with the English texts, no scroll bar, and 3 px less already scrolls.
- **Per-core load** under the CPU graph: one bar per logical processor, filling from the bottom, the threads of one physical core side by side and a small gap between cores, so one busy thread (one full bar) and parallel work (many medium bars) look different even when the total CPU % is the same. On the left, the load counted in full cores ("≈ 5.3 cores in use", the sum of the per-core loads). One `NtQuerySystemInformation` (`SystemProcessorPerformanceInformation`) call per second reads every core; which threads share a physical core comes once at startup from `GetLogicalProcessorInformationEx`. The space above and below the ruler before *Top CPU* / *Top RAM* went from 12 to 8, so the widget grows 8 units, not a full row.
- **Which process runs on which core** (Pulse running as administrator): the processes of *Top CPU* that use at least half a core, up to three, get a colour each (a small square next to the name, kept until the process drops below a quarter of a core) and every core bar is split into layers: from the bottom the share of each of them in its colour, the rest in the CPU colour. The row tooltip gives the same load in cores. Windows has no cheap call for "which process is on which core", so Pulse asks the kernel to sample, on every logical processor, the thread that is running: an ETW system-logger session with `SampledProfile` events every 10 ms (100 per second per core, instead of Windows' default 1 000) and the thread events to map threads to processes; the shares cover the last two seconds. The session is open only while there is a process to colour: with the PC idle there is none, so nothing is sampled. Without elevation the session is denied and the bars stay one colour, and the tooltip says why. `PULSE_NO_CORE_SAMPLING=1` turns it off. Measured cost with the session open, PC idle, sampling on and off alternated: Pulse 3.30% of one core with it, 3.34% without, private memory +1 MB.

- **Restart after a crash**: a new installer option, ticked by default, registers the Scheduled Task *Pulse restart*, which starts Pulse again ten seconds after Windows logs a crash of `Pulse.exe` (*Application Error* 1000, written even with Windows Error Reporting off), with the rights Pulse normally starts with. Pulse refuses a fourth restart within ten minutes, so a crash at startup doesn't loop.
- **Display driver changes**: when a display adapter goes away or comes back (a driver update, also from Windows Update, or a reset), Pulse stops reading the sensors at once and restarts itself five seconds later, with the same rights. On 10/10/2026 an NVIDIA driver update crashed a running Pulse four seconds after the update, inside LibreHardwareMonitor's NVML call.
- **Administrator rights right after installing or updating**: with *Start with administrator privileges*, a Pulse started without the rights (the launch at the end of the installer, which Inno Setup runs as a normal user even from an elevated setup, the Start menu, the desktop icon) starts the installer's *Pulse* task, which has the highest privileges and asks for no UAC, and closes: the elevated Pulse is up in about a second. Before, Pulse ran without the rights until the next logon. It happens only when everything matches (an administrator with UAC, the task enabled, with the highest privileges, for the same user and starting this very `Pulse.exe`) and at most once a minute; if no elevated Pulse shows up within 10 seconds, the first one carries on as before. `PULSE_NO_ADMIN_START=1` turns it off.

### Changed
- **English only**: every text of the widget, its tooltips, the tray and process menus and the dialogs is in English, and so are the numbers (decimal point: "5.0%", "13.3 MB/s"), the installer (one language) and the task descriptions. The internal arguments follow (`--restart`, `--after-crash`, `--appearance`, with `--aspetto` kept as an alias). So is the *Appearance...* window, which comes from Reticle: Reticle 0.6.1 gives it English texts.
- **`netsh` only on Wi-Fi**: the Wi-Fi name and signal came from a `netsh wlan show interfaces` started every 8 seconds even on a cable (31 ms of CPU each, 450 processes an hour for Defender to scan). It now runs only when the active connection is Wi-Fi, every 30 seconds.
- **NVIDIA GPUs read directly with NVML**: the six values the widget shows (load, temperature, core and memory clock, power, VRAM) come from the NVIDIA driver's own library instead of LibreHardwareMonitor's 41 sensors. A sensor read went from 84 ms (22 ms of CPU) every two seconds to 4.8 ms (1 ms of CPU) on the reference RTX 5070 Ti, with the same values. LibreHardwareMonitor still chooses the GPU to show, reads AMD and Intel GPUs, and takes over again on any NVML problem or with `PULSE_NO_NVML=1`. GPUs that are not shown (an integrated GPU next to a discrete one) are no longer read.
- **RAM explained with a bar**: the detail line of abbreviations ("Totale • Disp • Cache • Commit • PF") becomes, like the CPU one, the available memory on the left ("45.1 GB available") and under the graph the composition bar of Task Manager: in use, modified, cached (standby: kept ready, given back as soon as a program needs it) and free, in the RAM violet at decreasing strength. The tooltip explains each part and adds committed memory and page file usage. The memory lists come from `NtQuerySystemInformation` (`SystemMemoryListInformation`), readable without elevation; the widget keeps its height.
- **Reticle 0.6.1** (from 0.5.0): since 0.6.0 `ReticleWindow` no longer uses WPF's `WindowChrome` and answers `WM_NCCALCSIZE` and `WM_NCHITTEST` itself, the change that on Reticle's other adopters removed a full-window composition at every frame. The widget behaves as before: no caption drag area (`ShowCaption="False"`), the same 6-unit resize borders. 0.6.1 adds the English texts of the *Appearance* page (`TextLanguage="English"`).

### Fixed
- **Network throughput always read 0** (since v1.1.0, when `GetIfTable2` replaced the performance counters): the interface row was declared with ANSI strings, 864 bytes instead of 1352, so every row and field was read in the wrong place. It is now read at its real offsets, and only the physical adapters count: summing every interface that is up would have counted the same traffic four times on the reference PC, through the WFP and QoS filter interfaces of the same Ethernet card. Checked against Windows' own counters during a download: 13.31 against 13.36 MB/s.
- **The PC power estimate in the header was about 50 W too high** on the reference Ryzen: the GPU total also added the integrated GPU's power sensor, 53 W at idle, far more than its graphics draw and overlapping the CPU package power already counted. Only the GPU shown counts now.
- **The administrator Scheduled Task stopped Pulse after 72 hours**: created with `schtasks`, it kept the Task Scheduler defaults (72-hour limit, no start on battery, stop when unplugged). It now comes from a template with no limit that runs on battery too; reinstall to replace it.
- ***Start with administrator privileges* did nothing with *Install for me only***, the installer's default mode: without administrator rights Windows refuses to register a task with the highest privileges (access denied), the installer ignored the refusal, and no logon task was created, nor the restart task in its elevated form. The script that registers the tasks now asks for elevation when Windows refuses, once for both tasks; the same happens when a reinstall overwrites tasks registered elevated. In that mode the uninstaller asks for elevation too, once, since Windows also refuses to delete those tasks without it. Checked on test tasks from a non-elevated Windows PowerShell, as the installer runs it: one UAC prompt registered both, another overwrote an elevated task, a third deleted both with the uninstaller's command.
- **An installer option unticked in an update kept acting**: unticking *Start with administrator privileges* or *Restart Pulse if it closes because of an error* left the Scheduled Task an earlier install had registered, so Pulse still started elevated at logon (and, with the hand-off above, at every start) or still came back after a crash. The installer now removes the task of an unticked option: nothing happens if there is none, and a UAC prompt comes only for a *for me only* install whose task was registered elevated. *Start with Windows* is left alone, since the tray menu sets it too. Checked on test tasks: missing tasks and non-elevated ones went with no prompt, two elevated ones with one.
- ***Restart as administrator*** could start the elevated Pulse while the old one was still running, and the new one quit at once: it now waits for the old one to close.
- **A second Pulse crashed on its way out**: launched while another one ran, it quit as it should but released the single-instance mutex it didn't own, an unhandled exception logged as an application crash. Harmless until now; with the restart task it would have started a crash every 20 seconds.
- **Page file usage**: it read 100% on machines with much more RAM than page file, because it was estimated as committed memory minus RAM in use. It now comes from the page files themselves (`SystemPageFileInformation`): on the reference PC 0.12 of 4.0 GB, where the old line said "PF 100%".

### Performance (Ryzen 9 7950X reference, PC idle, same 622 × 502 window, v1.2.0 vs v1.3.0 alternated in two 45 s rounds on 11/10/2026, not elevated)
- CPU with the widget visible: 2.50% → **2.10%** of one core; hidden in the tray: 2.65% → **1.53%**. Hidden, the widget draws nothing and the figure is the data collection alone: NVML instead of LibreHardwareMonitor for the NVIDIA (22 → 1 ms of CPU every two seconds) and no reads of the GPU not shown. With the widget visible part of the saving goes to the new core and memory bars.
- Compositor (dwm) with the widget visible: CPU 6.6% → **2.5%**, GPU 1.67% → **0.57%**, lower in both rounds: the window without `WindowChrome` from Reticle 0.6 (Reticle 0.6 alone, in the same rounds: 4.1% and 0.60%). These are the noisiest figures, since other windows on screen move them too.
- GPU (3D engine) of Pulse: 0.05% → 0.04%; private memory: 130 → 131 MB; working set: 211 → 214 MB; handles: 887 → 916
- Cold start: ~590 → ~510 ms
- Exe size: 10.3 → 10.5 MB
- The v1.2.0 figures of this session are lower than the ones measured on 28/9 (below): compare within one section only.

## [v1.2.0] - 2026-09-28

### Changed
- **New look: the Reticle UI** (the author's WPF design system, v0.5.0): widget frame, Barlow / Barlow Condensed / Chakra Petch typography, dotted grid under the sparklines, *Top CPU* and *Top RAM* as framed panels, tabular digits so live values never shift their row.
- **No more layered transparent window**: `AllowsTransparency` and the blurred drop shadow are gone (each update redrew the shadow over the whole window and copied it back from the GPU). The window uses the Windows 11 Mica backdrop instead; Acrylic and opaque are one click away.
- Tray menu and process context menu follow the theme instead of hard-coded colours.
- CPU / GPU / NET / DSK series colours come from the theme tones; RAM violet and temperature amber stay fixed and get darker shades on the light theme.

### Added
- **Theme from the tray menu**: *Tema* → Scuro / Chiaro / Come Windows.
- **Appearance window** (*Aspetto...* in the tray menu, or `--aspetto` at startup): accent colour, theme, transparency, fill and density, applied live and saved in `%LOCALAPPDATA%\Pulse\reticle-theme.json`.
- `licenses/`: the SIL Open Font License 1.1 of the embedded fonts, installed next to `Pulse.exe` and shipped in the portable zip.

### Performance (Ryzen 9 7950X reference, PC idle, same 622 × 502 window, v1.1.0 vs v1.2.0 alternated in two 45 s rounds, not elevated)
- CPU with the widget visible: 5.8% → **4.9%** of one core; hidden in the tray: 4.8% → **4.4%**
- GPU (3D engine): 0.21% → **0.09%**; dedicated video memory: 34 → **17 MB**
- Private memory: 145 → **130 MB**; working set: 200 → 211 MB
- Cold start: ~506 → ~540 ms
- Exe size: 7.0 → 10.3 MB (embedded fonts)

### Build
- Reticle ships as two DLLs in `lib/Reticle` (v0.5.0), updated with `scripts/update-reticle.ps1 -Tag <tag>`: Pulse builds anywhere, CI included, with no access to the private Reticle repository.
- Release workflow: actions on Node 24 (checkout v7, setup-dotnet v6, upload-artifact v7).

## [v1.1.0] - 2026-05-24

### Added
- **Pulse branding**: custom ECG-heartbeat icon embedded as multi-resolution `.ico` (16/24/32/48/64/128/256 px). Window/taskbar/tray now show the icon. Widget header replaced "resources" with logo + "Pulse" label.
- **Right-click menu on top processes**: Apri cartella file • Proprietà file • Termina processo • Cerca online (Google)
- **Smooth Bezier sparklines** with cardinal-spline interpolation, vertical gradient fill, and animated halo dot on the last value
- Settings migration: existing `%APPDATA%\ResourceMonitor\settings.json` is copied to `%APPDATA%\Pulse\settings.json` on first launch

### Changed
- **Replaced `System.Diagnostics.PerformanceCounter` with Win32 native** (`GetSystemTimes`, `GetPerformanceInfo`, `GetIfTable2`, `NtQuerySystemInformation`, `CallNtPowerInformation`). Per-tick cost dropped from ~45 ms to <5 ms.
- Renamed `AssemblyName` to `Pulse` → executable is now `Pulse.exe` (was `ResourceMonitor.exe`)
- Sparkline now uses a pooled `Point[]` buffer + cached brushes — zero per-frame heap allocations
- Mutex / autostart registry value / settings folder all renamed to `Pulse`

### Performance (Ryzen 9 7950X reference, 120 s sample)
- Working Set: **211 → 194 MB** (−17 MB)
- Handles: **1052 → 850** (−202, ~25%)
- CPU avg: 4.59% → 3.98%
- CPU steady-state (>30 s): ~3% → **~1.5%** (−50%)
- Exe size: 9.5 → 7 MB
- RAM growth in 120 s: stable (no leak)

### Removed
- `System.Diagnostics.PerformanceCounter` NuGet package
- All `PerformanceCounter` instances and the corresponding warmup/disposal code

## [v1.0.0] - 2026-05-23

### Performance pass

After a structured measurement + refactor session:

| Metric | Before | After | Delta |
|---|---|---|---|
| Cold start | 287 ms | 274 ms | -13 ms |
| Working Set (RAM) | 222 MB | 207 MB | **-15 MB** |
| Private RAM | 146 MB | 144 MB | -2 MB |
| Handles | 1127 | 1057 | **-70** |
| Threads | 28 | 26 | -2 |
| RAM growth / 90s | +6.3 MB | +0.2 MB | leak eliminato |

Changes:
- Removed dependency on `System.Management` (replaced WMI CPU clock fallback with PerformanceCounter `Processor Information > Processor Frequency`)
- Removed `UseWindowsForms` — tray icon now uses pure Win32 P/Invoke (`Shell_NotifyIconW`) + WPF ContextMenu instead of `System.Windows.Forms.NotifyIcon`
- Disabled LibreHardwareMonitor Storage scanning (SSD temp opt-out by default; was costing ~150 ms init time and extra handles)
- Added env-var debug toggles: `PULSE_NO_LHM=1` to disable hardware monitoring entirely, `PULSE_NO_PROC=1` to disable top-process scanning
- Added `measure-deep.ps1` for structured profiling (cold start, RAM, CPU, handles, leaks)

### Added
- Sparklines for 60-second history per metric
- Top processes by CPU and RAM (updated every 5 s)
- Network details: ping, local + public IP, Wi-Fi SSID/signal
- Per-drive free space + total disk I/O
- System uptime + estimated total PC power draw
- Tray icon: show/hide, pin (topmost), auto-start, restart as admin, exit
- Persistent window position/size between sessions (JSON in `%APPDATA%\ResourceMonitor`)
- Inno Setup installer with optional admin-via-Scheduled-Task autostart
- GitHub Actions release workflow

### Hardware support
- AMD Ryzen (Zen 1-5): CPU temp/power/clock via Ryzen Master driver
- NVIDIA / AMD / Intel GPUs: usage/temp/power/VRAM via LibreHardwareMonitorLib
- SSD temp via SMART (no admin needed)
- Intel: documented path via HWiNFO64 (integration not yet implemented)

### Performance
- ~1.5-2% CPU idle (down from initial 6.77% after optimization)
- ~150 MB RAM (WPF + WinForms + LHM lib floor)
- ~270 ms cold start (with ReadyToRun)
- Single-file 9 MB exe + 1.5 MB native deps
