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
                         │  │  TextBlocks x12  │  ◄── strings  │
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
│  │ \settings.json   │  │  Net via PerfCounter)│                    │
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
│  │ NotifyIcon (WinForms) — tray icon + context menu     │          │
│  └──────────────────────────────────────────────────────┘          │
└────────────────────────────────────────────────────────────────────┘
```

## Service polling intervals

Different metrics update at different rates to balance freshness vs CPU cost:

| Service | Interval | Cost per tick | Why |
|---|---|---|---|
| MetricsService | 1 s | <10 ms | PerfCounter reads are cheap when filtered |
| HardwareMonitor (LHM) | 2 s | 10-50 ms | Polling all sensors; runs in background thread |
| ProcessMonitor | 5 s | 50-200 ms | Process.GetProcesses is expensive; background thread |
| NetworkMonitor | 2 s | <5 ms (ping) | Async ping; netsh subprocess every 8s on background |
| DriveMonitor | 30 s | <5 ms | DriveInfo is fast; can stall on sleeping HDDs (background) |

All slow work happens on `Task.Run` background threads. The UI thread only does the final text/sparkline updates via `Dispatcher.BeginInvoke`.

Performance log: ticks that exceed thresholds (100 ms metrics, 200 ms proc, 500 ms slow total) get logged to `%TEMP%\pulse-perf.log` for debugging UI freezes.

## Data flow

1. **MetricsService timer fires** → reads `Processor Information % Utility`, memory counters, disk Read/Write Bytes/sec, network Bytes/sec sums.
2. Raises `Updated` event.
3. `MainWindow.OnMetricsUpdated` updates TextBlocks and assigns new `double[]` arrays to Sparkline.Values (triggers `OnRender`).
4. **Slow tick fires** (separate timer) → background `Task.Run(_hw.Update())` (LHM), `_net.UpdateAsync()`, then marshals back to UI thread to update temp/clock/watt/IP labels.
5. `MetricsService.GpuExternal` and `VramUsedExternalGb` are set from `HardwareMonitor` outputs so the GPU sparkline still updates on the 1s tick.

## Sparkline rendering

`Controls/Sparkline.cs` is a `FrameworkElement` (not UserControl) that overrides `OnRender(DrawingContext)`. It:

- Reads `Values` (an enumerable of 60 doubles)
- Normalizes to `MaxValue` (or auto-scales if `AutoScale=true`)
- Builds a `StreamGeometry` for the fill (closed area to baseline)
- Builds a second `StreamGeometry` for the stroke line
- Draws fill (semi-transparent) + stroke (full opacity)

`Values` is registered with `FrameworkPropertyMetadataOptions.AffectsRender`, so any assignment re-triggers render. The MainWindow assigns a fresh `_metrics.CpuHistory.ToArray()` each tick.

## State persistence

Single JSON file at `%APPDATA%\ResourceMonitor\settings.json`:

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

Saved on `LocationChanged`, `SizeChanged`, on close, and on toggle of tray menu options.

## UI: Reticle

From v1.2.0 the widget is built on Reticle, the author's WPF design system (v0.5.0, as the two DLLs in `lib/Reticle`, updated with `scripts/update-reticle.ps1`):

- `App.xaml` merges `ReticleTheme` (the app defaults: dark, green accent, Mica, compact) and `ReticleControls`; `App.OnStartup` loads the user's choices with `ThemeManager.Initialize(new JsonThemeSettingsStore("Pulse"))`, i.e. `%LOCALAPPDATA%\Pulse\reticle-theme.json`.
- `MainWindow` is a `ReticleWindow` without caption bar (`ShowCaption="False"`) that hosts a `TickBorder` frame: the widget pattern of Reticle. No `AllowsTransparency`, no `DropShadowEffect`: the transparency is the Windows 11 backdrop (Mica by default, Acrylic or opaque from *Aspetto...*), composed by DWM instead of copied back from the GPU on every update.
- The tray menu switches the theme (*Tema*); *Aspetto...* opens Reticle's `AppearanceSettingsView`. Every change is applied live and saved.
- Series colours: CPU, GPU, NET and DSK use the theme tones (info, success, warning, danger); RAM violet and temperature amber are fixed categories, with darker shades on the light theme (`MainWindow.UpdateSeriesBrushes`).
- Reticle's animations and timers run only while the widget is on screen, so a widget hidden in the tray costs nothing on the UI side.

## Why WPF + WinForms tray icon

WPF doesn't include a `NotifyIcon`. Two ways to get one:

1. Third-party NuGet (e.g., `H.NotifyIcon.Wpf`) — adds dependency
2. Enable WinForms in the WPF project (`<UseWindowsForms>true</UseWindowsForms>`) and use `System.Windows.Forms.NotifyIcon`

We use option 2. It costs ~20-30 MB of RAM (WinForms assemblies loaded) and **blocks `PublishTrimmed`** (WinForms isn't trim-safe). Trade-off accepted for fewer external deps.

## Single instance

A named mutex `ResourceMonitor.SingleInstance` is created at startup. If already taken, the second launch silently exits.

## Build configuration

`ResourceMonitor.csproj` key flags:

- `<TargetFramework>net9.0-windows</TargetFramework>` — WPF on .NET 9
- `<UseWPF>true</UseWPF>`, `<UseWindowsForms>true</UseWindowsForms>`
- `<ServerGarbageCollection>false</ServerGarbageCollection>` — Workstation GC: lower memory floor
- `<PublishReadyToRun>true</PublishReadyToRun>` — ~270ms cold start, +4 MB exe size
- `<PublishReadyToRunComposite>true</PublishReadyToRunComposite>` — composite for better startup
- Framework-dependent + single-file deployment → ~9 MB exe + 1.5 MB native DLLs

## What got removed for performance

Earlier iterations measured ~6.77% idle CPU. Optimizations dropped it to ~1.5%:

1. **Per-core CPU counters** (16-32 PerfCounters) — we collected them but never displayed. Removed.
2. **GPU Engine counter sum** (100+ instances) — replaced by single LHM read for GPU load %.
3. **GPU Adapter Memory counter** — replaced by LHM `GPU Memory Total/Used` sensors.
4. **NetworkMonitor.UpdateWifi sync `netsh` call** — moved to `Task.Run` (was blocking UI up to 800 ms every 8 s).
5. **DriveMonitor.Update sync `DriveInfo.GetDrives`** — moved to `Task.Run` (could stall on sleeping HDDs).

See `docs/BUILD.md` for the measurement methodology and `measure.ps1`.
