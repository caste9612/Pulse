# Architecture

How Pulse is structured under the hood.

## High-level

```
                         ┌─────────────────────────────────────┐
                         │  MainWindow (ReticleWindow, Mica)   │
                         │                                     │
                         │  ┌──────────────────┐               │
                         │  │  Sparkline x5    │  ◄── values   │
                         │  │  (custom render) │      arrays   │
                         │  └──────────────────┘               │
                         │  ┌──────────────────┐               │
                         │  │  TextBlocks +    │  ◄── strings  │
                         │  │  lists (drives,  │               │
                         │  │  top processes)  │               │
                         │  └──────────────────┘               │
                         └────────────▲────────────────────────┘
                                      │ Dispatcher.BeginInvoke
                                      │ (UI thread updates)
┌─────────────────────────────────────┼──────────────────────────────┐
│                       App.OnStartup │                              │
│                                     │                              │
│  ┌──────────────────┐  ┌────────────┴─────────┐                    │
│  │ AppSettings JSON │  │ MetricsService       │ ◄── fires every 1s │
│  │ %APPDATA%\Pulse  │  │ (CPU%, RAM, Disk,    │     on dispatcher  │
│  │ \settings.json   │  │  Net via Win32)      │                    │
│  └──────────────────┘  └──────────────────────┘                    │
│                                                                    │
│  ┌──────────────────┐  ┌──────────────────────┐                    │
│  │ HardwareMonitor  │  │ ProcessMonitor       │                    │
│  │ (LHM lib, 2s)    │  │ (Process.*, 5s)      │                    │
│  └─────┬────────────┘  └──────────────────────┘                    │
│        │                                                           │
│        │ Ring-0 via signed driver                                  │
│        ▼                                                           │
│  ┌──────────────────────────────┐                                  │
│  │ Kernel driver (Ring 0)       │  AMDRyzenMasterDriverV31 (AMD)   │
│  │ exposes MSR / SMU / NVAPI    │  or LHM's own driver (Intel/AMD) │
│  └──────────────────────────────┘                                  │
│                                                                    │
│  ┌──────────────────┐  ┌──────────────────────┐                    │
│  │ NetworkMonitor   │  │ DriveMonitor         │                    │
│  │ (ping, IPs, 2s)  │  │ (DriveInfo, 30s)     │                    │
│  └──────────────────┘  └──────────────────────┘                    │
│                                                                    │
│  ┌──────────────────────────────────────────────────────┐          │
│  │ TrayIcon (Win32 Shell_NotifyIconW) + WPF ContextMenu │          │
│  └──────────────────────────────────────────────────────┘          │
└────────────────────────────────────────────────────────────────────┘
```

## Service polling intervals

Different metrics update at different rates to balance freshness vs CPU cost:

| Service | Interval | Cost per tick | Why |
|---|---|---|---|
| MetricsService | 1 s | ~5 ms | Direct Win32 calls through `NativeMetrics`; runs on the UI thread (`DispatcherTimer`) |
| HardwareMonitor (LHM) | 2 s | 10-50 ms | Polling all sensors; runs in background thread |
| ProcessMonitor | 5 s | 50-200 ms | Process.GetProcesses is expensive; background thread |
| NetworkMonitor | 2 s | <5 ms (ping) | Async ping to 8.8.8.8; adapter scan and `netsh` (every 8 s) on a background thread; public IP every 10 min |
| DriveMonitor | 30 s | <5 ms | DriveInfo is fast; can stall on sleeping HDDs (background) |

The 1 s tick is cheap enough to run on the UI thread. All slower work happens on `Task.Run` background threads, and the UI thread only does the final text/sparkline updates.

Performance log: ticks that exceed thresholds (50 ms metrics tick, 200 ms hardware/network/process ticks, 500 ms whole slow tick) get logged to `%TEMP%\pulse-perf.log` for debugging UI freezes.

## Data flow

1. **MetricsService timer fires** (1 s, UI thread) → `NativeMetrics` reads the CPU times (`GetSystemTimes`), memory (`GlobalMemoryStatusEx`, `GetPerformanceInfo`), the system-wide I/O byte counters (`NtQuerySystemInformation`), the network interface table (`GetIfTable2`) and the CPU clock (`CallNtPowerInformation`). CPU % and the disk/network rates come from the delta with the previous sample.
2. Raises `Updated` event.
3. `MainWindow.OnMetricsUpdated` updates TextBlocks and assigns a fresh `ToArray()` of each 60-value history to `Sparkline.Values` (triggers `OnRender`).
4. **Slow tick fires** (2 s, separate timer) → background `Task.Run(_hw.Update)` (LHM), then `_net.UpdateAsync()`, then back on the UI thread to update temp/clock/watt/IP labels.
5. `MetricsService.GpuExternal` and `VramUsedExternalGb` are set from `HardwareMonitor` outputs so the GPU sparkline still updates on the 1s tick.
6. **Process tick** (5 s) and **drive tick** (30 s) scan on `Task.Run`, then replace the items of the *Top CPU* / *Top RAM* panels and of the drive list.

## Sparkline rendering

`Controls/Sparkline.cs` is a `FrameworkElement` (not UserControl) that overrides `OnRender(DrawingContext)`. It:

- Reads `Values` (an enumerable of 60 doubles) into a `Point[]` buffer reused between renders
- Normalizes to `MaxValue` (100 for CPU, RAM and GPU) or auto-scales to the highest value (`AutoScale="True"` for NET and DSK)
- Smooths the line: a cardinal spline (tension 0.5) written as cubic Bezier segments
- Builds a `StreamGeometry` for the fill (closed area to baseline) and a second one for the stroke line
- Draws the fill (vertical gradient from `FillOpacity` to transparent), the stroke and a dot with a halo on the last value (`ShowEndDot`)

The gradient and halo brushes are cached and rebuilt only when the colour changes. The dotted grid under the curve is not drawn here: it is the `Reticle.Brush.Dots` background of the hosting border (`SparkBed` in `MainWindow.xaml`).

`Values` is registered with `FrameworkPropertyMetadataOptions.AffectsRender`, so any assignment re-triggers render. The MainWindow assigns a fresh `ToArray()` of each history every tick.

## State persistence

Window state lives in a single JSON file at `%APPDATA%\Pulse\settings.json` (on first launch it is copied from `%APPDATA%\ResourceMonitor\settings.json`, the pre-v1.1.0 location, if only that one exists):

```json
{
  "Left": 2654, "Top": 59,
  "Width": 670, "Height": 519,
  "IsVisible": true,
  "Topmost": true,
  "StartHidden": false,
  "AutoStart": false
}
```

Saved on `LocationChanged`, `SizeChanged`, on close, when the widget is hidden or shown, and on toggle of tray menu options.

The appearance (theme, accent, transparency, fill, density) is saved by Reticle in `%LOCALAPPDATA%\Pulse\reticle-theme.json` (see below). *Avvia con Windows* writes the `Pulse` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

## UI: Reticle

From v1.2.0 the widget is built on Reticle, the author's WPF design system (v0.6.0, as the two DLLs in `lib/Reticle`, updated with `scripts/update-reticle.ps1`):

- `App.xaml` merges `ReticleTheme` (the app defaults: dark, green accent, Mica, compact) and `ReticleControls`; `App.OnStartup` loads the user's choices with `ThemeManager.Current.Initialize(new JsonThemeSettingsStore("Pulse"))`, i.e. `%LOCALAPPDATA%\Pulse\reticle-theme.json`.
- `MainWindow` is a `ReticleWindow` without caption bar (`ShowCaption="False"`) that hosts a `TickBorder` frame: the widget pattern of Reticle. No `AllowsTransparency`, no `DropShadowEffect`: the transparency is the Windows 11 backdrop (Mica by default, Acrylic or opaque from *Aspetto...*), composed by DWM instead of copied back from the GPU on every update.
- Since Reticle 0.6.0 there is no WPF `WindowChrome` either: `ReticleWindow` answers `WM_NCCALCSIZE` (the whole window is client area) and `WM_NCHITTEST` (6-unit resize borders) itself. With `ShowCaption="False"` its drag area is 0 high, so dragging the header and double-clicking it stay Pulse's own handlers.
- The tray menu switches the theme (*Tema*); *Aspetto...* opens Reticle's `AppearanceSettingsView`. Every change is applied live and saved.
- Series colours: CPU, GPU, NET and DSK use the theme tones (info, success, warning, danger); RAM violet and temperature amber are fixed categories, with darker shades on the light theme (`MainWindow.UpdateSeriesBrushes`).
- Reticle's animations and timers run only while the widget is on screen. Pulse's own polling keeps running while the widget is hidden in the tray, so the values are current when it comes back: hidden costs less CPU than visible, not zero (see the v1.2.0 figures in the README).

## Tray icon without WinForms

WPF doesn't include a `NotifyIcon`. Three ways to get one:

1. Third-party NuGet (e.g., `H.NotifyIcon.Wpf`) — adds dependency
2. Enable WinForms in the WPF project (`<UseWindowsForms>true</UseWindowsForms>`) and use `System.Windows.Forms.NotifyIcon` — what Pulse did before v1.0.0; loading the WinForms assemblies cost 20-30 MB of RAM
3. Call the Windows shell directly — what Pulse does

`Services/TrayIcon.cs` registers the icon with `Shell_NotifyIconW` (`NOTIFYICON_VERSION_4`) and gets its messages through a message-only window (an `HwndSource` parented to `HWND_MESSAGE`). The icon is the exe's own, loaded with `ExtractIconEx`. Left click shows or hides the widget, double click brings it back, right click opens a WPF `ContextMenu` at the mouse position: `App.PopulateMenu` rebuilds it at every opening, and it takes its style from the Reticle theme.

## Single instance

A named mutex `Pulse.SingleInstance` is created at startup (`ResourceMonitor.SingleInstance` before v1.1.0). If already taken, the second launch silently exits, so a build started while another Pulse is running doesn't start: close the other one first.

## Build configuration

`ResourceMonitor.csproj` key flags:

- `<TargetFramework>net9.0-windows</TargetFramework>`, `<UseWPF>true</UseWPF>` — WPF on .NET 9, no WinForms
- `<AssemblyName>Pulse</AssemblyName>` — the exe is `Pulse.exe`; the namespace stays `ResourceMonitor`
- `<ServerGarbageCollection>false</ServerGarbageCollection>` — Workstation GC: lower memory floor
- `ConcurrentGarbageCollection`, `TieredCompilation`, `TieredPGO` set to `true` — the .NET defaults, made explicit
- `<PublishReadyToRun>true</PublishReadyToRun>` (Release) — precompiled IL: ~30% less JIT at cold start, +4 MB exe size
- `<PublishReadyToRunComposite>true</PublishReadyToRunComposite>` (Release) — composite for better startup
- `LibreHardwareMonitorLib` 0.9.6 from NuGet; `Reticle.Wpf` and `Reticle.Tokens` referenced as DLLs from `lib/Reticle`
- Framework-dependent + single-file deployment → ~10 MB exe (Reticle and its fonts included) + 1.5 MB native DLLs

## What got removed for performance

Earlier iterations measured ~6.77% idle CPU. Optimizations dropped it to ~1.5%:

1. **Per-core CPU counters** (16-32 PerfCounters) — we collected them but never displayed. Removed.
2. **GPU Engine counter sum** (100+ instances) — replaced by single LHM read for GPU load %.
3. **GPU Adapter Memory counter** — replaced by LHM `GPU Memory Total/Used` sensors.
4. **NetworkMonitor.UpdateWifi sync `netsh` call** — moved to `Task.Run` (was blocking UI up to 800 ms every 8 s).
5. **DriveMonitor.Update sync `DriveInfo.GetDrives`** — moved to `Task.Run` (could stall on sleeping HDDs).

Later releases removed whole layers:

- v1.0.0: WinForms (the tray icon, see above) and `System.Management` (WMI)
- v1.1.0: `PerformanceCounter` altogether, replaced by the Win32 calls of `NativeMetrics` (~45 ms → ~5 ms per tick)
- v1.2.0: the layered transparent window (`AllowsTransparency`) and its drop shadow, replaced by the Mica backdrop

See `docs/BUILD.md` for the measurement methodology and `measure-deep.ps1`.
