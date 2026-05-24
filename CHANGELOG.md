# Changelog

All notable changes to Pulse will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
