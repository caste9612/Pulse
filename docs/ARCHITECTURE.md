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

Performance log: ticks that exceed thresholds (50 ms metrics tick, 200 ms hardware/network/process ticks, 500 ms whole slow tick) get logged to `%TEMP%\pulse-perf.log` for debugging UI freezes. The same file gets a line when the core sampling of an elevated Pulse can't start or delivers no samples (see "Which process runs on which core").

## Data flow

1. **MetricsService timer fires** (1 s, UI thread) → `NativeMetrics` reads the CPU times (`GetSystemTimes`), the times of every logical processor (`NtQuerySystemInformation`, `SystemProcessorPerformanceInformation`), memory (`GlobalMemoryStatusEx`, `GetPerformanceInfo` for the committed memory, `NtQuerySystemInformation` for the memory lists, `SystemMemoryListInformation`, and the page files, `SystemPageFileInformation`), the system-wide I/O byte counters (`NtQuerySystemInformation`, `SystemPerformanceInformation`), the network interface table (`GetIfTable2`) and the CPU clock (`CallNtPowerInformation`). CPU %, the per-core loads and the disk/network rates come from the delta with the previous sample. Which logical processors share a physical core is read once, at startup (`GetLogicalProcessorInformationEx`). With Pulse elevated, `CoreSampler` adds which processes ran on each core (see below).
2. Raises `Updated` event.
3. `MainWindow.OnMetricsUpdated` updates TextBlocks, assigns a fresh `ToArray()` of each 60-value history to `Sparkline.Values` and the fresh per-core array to `CoreBars.Values` (both trigger `OnRender`).
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

`Controls/CoreBars.cs`, under the CPU sparkline, works the same way: one bar per logical processor from the latest sample (no history, no animation), the threads of one physical core side by side, a track in the series colour at 10 % and the fill from the bottom. Gaps and edges are rounded to device pixels, so the narrow bars stay sharp at any scale.

`Controls/MemoryBar.cs`, under the RAM sparkline, is the composition bar of Task Manager drawn the same way: in use, modified, standby (shown as "in cache") and free from the left, in the RAM colour at 100 %, 60 %, 30 % and 10 % (the free part is the track), with a 1 px gap between parts. "In use" is what the memory lists leave: total minus free, modified and standby.

## Which process runs on which core

Windows keeps no cheap answer to "which process is on which core": the scheduler moves threads between cores all the time. `Services/CoreSampler.cs` asks the kernel for samples instead, the way profilers do:

- It opens a real-time ETW session in system-logger mode (`EVENT_TRACE_SYSTEM_LOGGER_MODE`, so it doesn't take the single *NT Kernel Logger*), named `Pulse core sampling`, with the `PROFILE` and `THREAD` flags, only while there is a process to colour: `MainWindow` asks for it at each process tick (`MetricsService.SetCoreSampling`), and the session opens and closes off the UI thread. With the PC idle there is none, so nothing is sampled. Only an elevated Pulse can open it; otherwise the core tooltip says that the colours need the administrator.
- The profiling interval is global: Pulse sets it to 10 ms (100 samples per second per logical processor, instead of Windows' default 1 ms) and puts the previous value back when the session closes, unless someone else changed it meanwhile. If it can't set it, it closes the session rather than run at ten times the samples.
- A background thread runs `ProcessTrace`. Each `SampledProfile` event says which thread was running on which processor; the thread events (`DCStart` for the threads alive at startup, then `Start` and `End`) map threads to processes, and a thread seen nowhere is looked up once with `OpenThread` + `GetProcessIdOfThread`.
- The samples are counted per core in four columns: the three tracked processes and everything else; idle samples don't count. Every second `MetricsService` takes the shares of the last two seconds, and `CoreBars` draws them as layers inside each bar, whose height still comes from the per-core times of `NtQuerySystemInformation`.
- `MainWindow` chooses the tracked processes at each process tick: the processes of *Top CPU* that use at least half a core, up to three. A process keeps its colour until it drops below a quarter of a core, so one hovering around the threshold doesn't blink.
- The session is stopped when the last tracked process goes and on exit (`ExitApp`, with `ProcessExit` as a fallback). If Pulse is killed while sampling the session stays open without a consumer; an elevated Pulse closes it at its next start.
- `PULSE_NO_CORE_SAMPLING=1` turns it off, like `PULSE_NO_LHM` and `PULSE_NO_PROC`.

Cost of an open session, measured on the Ryzen 9 7950X reference with the PC idle, an elevated build and the sampling on and off alternated in two 40 s rounds: Pulse's own CPU 3.30% of one core with it, 3.34% without (no measurable difference), private memory +1 MB. The kernel side (about 3,200 samples per second on 32 logical processors) didn't show in the system-wide busy time, whose noise on an idle PC was larger (1.5–2.9% in both cases).

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

A named mutex `Pulse.SingleInstance` is created at startup (`ResourceMonitor.SingleInstance` before v1.1.0). If already taken, the second launch silently exits, so a build started while another Pulse is running doesn't start: close the other one first. It exits without touching the mutex: until v1.2.0 `OnExit` released it anyway, and the second Pulse crashed on its way out (a crash the restart task below would have restarted forever).

The exceptions are the restarts, whose previous process may still be on its way out: with `--riavvio` (after a display driver change, or *Riavvia come amministratore*, which before could see the old Pulse still running and quit) or `--dopo-crash` (from the restart task) the new Pulse waits up to 10 s for the mutex.

## Staying up

Two things closed Pulse for good before v1.3.0, and both now bring it back:

- **A display driver change.** `Services/GpuWatch.cs` registers for display adapter arrival and removal with `CM_Register_Notification` (no window, nothing to do until it fires). On the first notice `HardwareMonitor.Pause()` stops every sensor read, also between two hardware items of a running update, because an NVML call with the old driver's handle crashes the process (a native access violation .NET can't catch: what happened on 10/10/2026, four seconds after an NVIDIA driver update). Five seconds after the last notice Pulse restarts itself: `App.RestartAfterGpuChange` saves the window, closes the tray icon and the metrics (the core sampling session too), releases the mutex and starts `Pulse.exe --riavvio`, which inherits the same token, so an elevated Pulse stays elevated. LibreHardwareMonitor is not closed: closing it would call into NVML too. A new process is also simpler and safer than reopening NVML inside the old one, which would keep the `nvml.dll` it loaded with the old driver.
- **A crash.** The installer's `Pulse riavvio` task (see `docs/INSTALLER.md`) starts `Pulse.exe --dopo-crash` ten seconds after Windows logs the crash; `SettingsService.AllowCrashRestart` keeps the last restarts in `%APPDATA%\Pulse\riavvii-dopo-crash.txt` and refuses a fourth one within ten minutes.

Both were checked on test builds with a simulated driver change and a deliberate crash: the restarted Pulse came back in 0.7 s and in 11 s, at the same place on screen; four crashes in a row gave three restarts and a refusal.

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

1. **Per-core CPU counters** (16-32 PerfCounters) — we collected them but never displayed. Removed. The per-core bars that came later read every core with one `NtQuerySystemInformation` call per tick.
2. **GPU Engine counter sum** (100+ instances) — replaced by single LHM read for GPU load %.
3. **GPU Adapter Memory counter** — replaced by LHM `GPU Memory Total/Used` sensors.
4. **NetworkMonitor.UpdateWifi sync `netsh` call** — moved to `Task.Run` (was blocking UI up to 800 ms every 8 s).
5. **DriveMonitor.Update sync `DriveInfo.GetDrives`** — moved to `Task.Run` (could stall on sleeping HDDs).

Later releases removed whole layers:

- v1.0.0: WinForms (the tray icon, see above) and `System.Management` (WMI)
- v1.1.0: `PerformanceCounter` altogether, replaced by the Win32 calls of `NativeMetrics` (~45 ms → ~5 ms per tick)
- v1.2.0: the layered transparent window (`AllowsTransparency`) and its drop shadow, replaced by the Mica backdrop

See `docs/BUILD.md` for the measurement methodology and `measure-deep.ps1`.
