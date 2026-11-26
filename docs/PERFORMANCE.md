# Pulse performance

What Pulse costs while it runs, measured release by release, and the choices that keep it small. The
[README](../README.md) has the short version.


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
