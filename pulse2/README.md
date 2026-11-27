# Pulse 2

Pulse rewritten in C++ on Windows alone: no external libraries, no exceptions, no C++ standard library, one exe of
under 1 MB with its fonts inside. Work in progress on the `pulse2` branch; the Pulse that is released and installed is
still the WPF one in `ResourceMonitor/`.

It draws the panel of Pulse 1.4 (one text column, the level behind each value, mirrored network and disk graphs,
programs instead of processes) with Direct2D rendered by the CPU, on an opaque background: in the prototype measured on
26/11/2026 the GPU path cost about 60 MB for its driver alone, the software one about 14.

## What is there

- **Readings that need no rights**, each checked against Windows' own figures:
  - every second, on the window thread: CPU and every logical core (with the physical core it belongs to), memory with
    its page lists, committed memory and page file, disk and network rates (physical adapters only);
  - on a worker thread: programs every 5 s (one call for all processes, added up by name), an NVIDIA GPU through NVML
    every 2 s, the average CPU speed as in Task Manager (PerfLib V2, with the clock Windows reports as a fallback),
    ping, adapter and local address (when Windows reports a change and once a minute), drives every 30 s, the public
    address every ten minutes.
- **The window**: no frame, dragged by its header, resized from its left and right edges; its height is always the
  content's. Header buttons: fit, corner, always on top, hide. Light and dark theme, or the one Windows uses.
- **Tray icon** with its menu (show or hide, theme, always on top, start with Windows, exit), **settings** in
  `%APPDATA%\Pulse2\settings.txt`, a **single instance**.
- **Tooltips** for the cores, the memory, the GPU, the power estimate, the header buttons and each program row, and the
  **menu of a program** (open file location, properties, end, search online). *End* closes every process of the
  program, after a confirmation that says how many.

## What is missing

- **CPU temperature and power, and the colours of the programs in the core bars**: they need administrator rights, and
  will come from a separate sensors process, the only elevated one. Until then the temperature reads `n/a`.
- GPUs other than NVIDIA, the Wi-Fi name and signal, the SSD temperature.
- The watch on display driver changes. A crashing NVML call is caught (`__try`) and turns the GPU readings off until
  the next start, instead of taking the process down.
- The accent, transparency and density of Pulse 1.x's *Appearance* window; menus and tooltips are the system's own, not
  drawn in the panel's style; the corner button opens a menu, not the four arrows.
- Vertical scrolling: the window is as high as its content, so on a short screen the bottom is cut.
- An installer, and the passage of the settings from Pulse 1.x.

## Build and run

`build.cmd` needs Visual Studio with the 64-bit C++ compiler and writes `out\Pulse2.exe`.

| Argument | |
|---|---|
| `--pos <x> <y>`, `--width <n>` | position and width, instead of the saved ones |
| `--theme dark\|light` | theme, instead of the saved one |
| `--nosave` | a test copy: reads and writes no settings, no single instance |
| `--noactivate`, `--seconds <n>` | don't take the focus; close after n seconds |
| `--shot <file.bmp>` | read for a second, render one frame off screen, save it and exit |
| `--dump <file.txt>` | read for a second and write the readings, the regions and their tooltips as text |

`PULSE_NO_PUBLIC_IP=1` keeps Pulse 2 from asking for the public address. The log is `%TEMP%\pulse2.log`.

## First figures

A look, not a measurement (one run of 70 s on the reference PC, 27/11/2026, next to the installed Pulse): first frame
after about 50 ms, 37 MB of private memory, about 1% of one core in its first minute. Pulse 1.4.0 measured 132 MB and
1.65%. Most of the 37 MB arrive with the NVIDIA library.
