# Build & Development

How to build Pulse from source.

## Prerequisites

- Windows 10 / 11
- .NET 9 SDK ([download](https://dotnet.microsoft.com/download/dotnet/9.0))
- (Optional) Inno Setup 6+ to build the installer ([download](https://jrsoftware.org/isdl.php) or `winget install JRSoftware.InnoSetup`)
- (Optional) Visual Studio 2022/2026 or VS Code with C# extension

## Clone

```bash
git clone https://github.com/<user>/pulse
cd pulse
```

## Debug build

```bash
cd ResourceMonitor
dotnet build
```

Output: `bin/Debug/net9.0-windows/ResourceMonitor.exe` (~150 KB, requires DLLs alongside).

Run from VS / Rider / `dotnet run` for a debugging session.

## Release build

```bash
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o ../dist
```

Flags explained:
- `-c Release` — optimizations on, no debug symbols inlined
- `-r win-x64` — Windows x64 runtime identifier
- `--self-contained false` — assumes .NET 9 Desktop Runtime is installed (saves ~70 MB exe)
- `-p:PublishSingleFile=true` — bundles managed DLLs into a single exe (native LHM DLLs stay separate)

Result in `dist/`:
- `ResourceMonitor.exe` — ~9 MB
- `libMonoPosixHelper.dll`, `MonoPosixHelper.dll` — native deps of LibreHardwareMonitorLib (~1.5 MB)

`ResourceMonitor.csproj` Release config also enables:
- `PublishReadyToRun=true` — precompiles managed IL to native code → faster cold start (~270 ms)
- `PublishReadyToRunComposite=true` — better R2R packaging

## Project layout

```
ResourceMonitor/
├── ResourceMonitor.csproj         project file
├── App.xaml / App.xaml.cs         entry point + tray icon
├── MainWindow.xaml / .cs          main UI + drag/resize/persist
│
├── Services/
│   ├── MetricsService.cs          1s tick: CPU/RAM/Disk/Net via PerfCounter
│   ├── HardwareMonitor.cs         2s tick: LHM wrapper (temps/clocks/watts)
│   ├── ProcessMonitor.cs          5s tick: top processes by CPU/RAM
│   ├── NetworkMonitor.cs          2s tick: ping, IPs, Wi-Fi
│   ├── DriveMonitor.cs            30s tick: free space per drive
│   ├── SettingsService.cs         JSON load/save (%APPDATA%)
│   └── PerfLog.cs                 logs slow ticks (debug)
│
└── Controls/
    └── Sparkline.cs               custom-render sparkline chart
```

## Performance measurement

The repo includes `measure.ps1`, which:

1. Kills running Pulse instances
2. (Optionally) clears the single-file extraction cache for true cold-start timing
3. Launches and measures:
   - `WaitForInputIdle` time (message loop ready)
   - Working set + private memory after 25 s settle
   - Average CPU idle % over 30 s sample
   - Thread count
4. Appends to `perf-history.csv`

Usage:

```powershell
./measure.ps1 -Label "baseline" -ClearCache
```

History from refactoring (Ryzen 9 7950X reference):

| Label | Exe MB | RAM (MB) | CPU idle % | Notes |
|---|---|---|---|---|
| Initial | 5.07 | 158 | 6.77 | All PerfCounters including per-core CPU |
| r2-no-percore | 5.08 | 157 | 3.18 | Removed 32 per-core CPU counters |
| r3-no-gpu-perfctr | 5.08 | 157 | 1.35 | GPU stats via LHM only |
| r4-r2r | 9.4 | 155 | ~2 | + ReadyToRun precompile |
| current | 9.4 | 146-155 | 1.5-3 | + Wifi/Drive offload to background |

## Diagnostic sensor dump

To see what LibreHardwareMonitorLib exposes on a new system:

1. Uncomment `try { DumpSensors(); } catch { }` in `HardwareMonitor.Update()`
2. Rebuild + run as admin
3. Open `%TEMP%\pulse-sensors.txt`
4. Look for non-zero values and copy any new sensor names to the filters

Useful when adding support for new CPU generations.

## Perf log

Pulse writes to `%TEMP%\pulse-perf.log` whenever a tick exceeds its threshold (100 ms metrics, 200 ms procs, 500 ms slow total). Empty file = no UI freezes detected.

## Building the installer

Requires Inno Setup 6+:

```powershell
winget install JRSoftware.InnoSetup
```

Build the release first (above), then:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer/pulse.iss
```

Output: `installer/Output/Setup-Pulse-X.Y.Z.exe`.

See [INSTALLER.md](INSTALLER.md) for what the installer does.

## CI/CD

`.github/workflows/release.yml` builds and packages on every `vX.Y.Z` tag:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow runs on Windows runners, calls `dotnet publish` + `iscc.exe`, and uploads both `Setup-Pulse-vX.Y.Z.exe` and `Pulse-vX.Y.Z-portable.zip` to a draft GitHub Release.

## Debugging the installed instance

After install, the exe lives at `%LocalAppData%\Programs\Pulse\ResourceMonitor.exe` (or `%ProgramFiles%\Pulse\` if installed system-wide).

To attach a debugger:

1. Launch the installed Pulse (or start with `--no-tray` from a console for stdout)
2. In Visual Studio: Debug → Attach to Process → ResourceMonitor.exe

Settings + logs paths:

- Settings: `%APPDATA%\ResourceMonitor\settings.json`
- Perf log: `%TEMP%\pulse-perf.log`
- Sensor dump (if enabled): `%TEMP%\pulse-sensors.txt`

## Contributing

PRs welcome. Conventions:

- 4-space indent (C# default)
- File-scoped namespaces (`namespace X;`)
- Nullable enabled — use `?` and pattern matching
- No comments explaining *what* — explain *why* if non-obvious
- Match existing structure: services in `Services/`, controls in `Controls/`
- Test on at least one AMD and one Intel system before submitting CPU-related changes
