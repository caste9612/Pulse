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
- **Five graphs, one style**: the same curve, line width, fade and dot on the latest value for CPU, memory, GPU,
  network and disk. The curve passes through the readings and between two of them never goes above the higher or
  below the lower, so a spike looks as high as it was. Network and disk are mirrored, in above the axis and out below
  it; with nothing passing the two lines lie on the axis as one. (Pulse 1.4 draws these two with straight lines and
  a flat fill.)
- **The window**: no frame, dragged by its header, resized from its left and right edges; its height is always the
  content's. Dragged near an edge of the screen's work area it stops 12 units from it. Header buttons: fit, corner
  (four arrows open under it, one per corner of the screen), always on top, hide. Light and dark theme, or the one
  Windows uses; menus and tooltips follow it.
- **Tray icon** with its menu (show or hide, theme, always on top, start with Windows, exit) and the three loads in
  its tooltip, **settings** in `%APPDATA%\Pulse2\settings.txt`, a **single instance**.
- **Drawing only when someone can see it**: hidden in the tray, with the session locked or the display off, Pulse 2
  keeps reading, so the last minute is there when it comes back, but draws nothing.
- **Tooltips** for the cores, the memory, the GPU, the power estimate, the header buttons and each program row, and the
  **menu of a program** (open file location, properties, end, search online). *End* closes every process of the
  program, after a confirmation that says how many.

## The sensors process

What needs administrator rights is not in the panel: `sensors/` is a second, small program (`Pulse2.Sensors.exe`, .NET,
no window), the only one meant to run elevated. It reads the CPU's registers with LibreHardwareMonitor and the PawnIO
driver (temperature, package power, the clock of the fastest core) and samples the cores with the kernel, with the same
`CoreSampler.cs` as Pulse 1.x, compiled from `ResourceMonitor/`, to know which program runs on which core.

The two talk through a block of shared memory, `Local\Pulse2.Shared`, that the panel creates (`src/shared.h`): the
panel writes which programs of *Top CPU* it wants in colour (those above half a core, up to three, with their process
IDs), the sensors process answers with the three CPU values and, for every logical core, the share of each program.
Each side writes only its own half, with a counter that is odd while a write is in progress. Without the sensors
process, or with one that is not elevated, the panel works as before: the temperature reads `n/a` and the core bars
have one colour; the tooltip says what is missing. When the panel is closed or silent for five seconds the sensors
process reads nothing and keeps no kernel session open.

Checked on the reference PC on 27/11/2026, next to the installed Pulse:

- without rights: with `--fake` (made-up values, to test the link) and with the real process not elevated, where
  everything stays `n/a` with no errors;
- **as administrator**, with the panel not elevated: CPU temperature (60°C, with its line in the graph), package power
  (112 W) and the fastest core's clock (5.33 GHz) reached the panel, and the core bars took the colours of three
  programs above half a core, one of three processes and one of two, each process tied to its own core: three, two
  and one coloured bars, counted on the capture. The kernel session opened with them and closed by itself twelve
  seconds after they stopped, the system's profiling interval went back to 1 ms, and the installed Pulse kept its own
  session. The sensors process had 10 MB of private memory and used 0.23 s of CPU in 45 s.

## What is missing

- The Scheduled Task that starts the sensors process elevated at logon: for now it is started by hand.
- GPUs other than NVIDIA, the Wi-Fi name and signal, the SSD temperature.
- The watch on display driver changes. A crashing NVML call is caught (`__try`) and turns the GPU readings off until
  the next start, instead of taking the process down.
- The accent, transparency and density of Pulse 1.x's *Appearance* window. Menus and tooltips are the system's own,
  dark or light with the theme, not drawn in the panel's style.
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
| `--shot <file.bmp>` | read for a second, render one frame off screen, save it and exit; `--demo-ui` adds a hovered row and the open corner arrows, `--demo-data` uses the made-up data of `--bench`, `--dpi <n>` another scale (144 is 150%) |
| `--bench <n>` | draw n frames in a row with the same made-up data and log the CPU each one cost |
| `--dump <file.txt>` | read for a second and write the readings, the regions and their tooltips as text |

The sensors process builds with `dotnet build sensors` and takes `--fake` (made-up values instead of sensors and
sampling) and `--seconds <n>`; its log is `%TEMP%\pulse2-sensors.log`.

`PULSE_NO_PUBLIC_IP=1` keeps Pulse 2 from asking for the public address, `PULSE_NO_NVML=1` from loading the NVIDIA
library. The log is `%TEMP%\pulse2.log`.

## First figures

Looks, not measurements: single runs on the reference PC next to the installed Pulse, on 27 and 28/11/2026, with the PC
in use.

- First frame after about 50 ms.
- **CPU**: hidden in the tray 0.12% of one core, which is the readings alone; visible 0.27%. Before the drawing was
  reworked it was about 1.2% visible: one frame cost 6.1 ms of CPU and now costs 1.4 (`--bench`). Pulse 1.4.0
  measured 1.65% visible.
- **Private memory**: 41 MB, steady over three and a half minutes; 19.5 MB with `PULSE_NO_NVML=1`, so about 21 MB
  arrive with the NVIDIA library. Pulse 1.4.0 measured 132 MB.
- The sensors process, elevated: 10 MB and 0.23 s of CPU in 45 s.
