# Hardware Support

Which sensors work on which hardware, and why some need extra software.

## TL;DR

| Sensor | Source | Requires |
|---|---|---|
| CPU usage % | `GetSystemTimes` (Win32) | nothing |
| Per-core load | `NtQuerySystemInformation` (`SystemProcessorPerformanceInformation`, all cores in one call); threads per physical core from `GetLogicalProcessorInformationEx`, once at startup | nothing; up to 64 logical processors (one processor group) |
| Which process runs on each core | ETW kernel sampling: `SampledProfile` events every 10 ms per logical processor, in a real-time system-logger session (see ARCHITECTURE) | **admin** |
| CPU clock GHz | LibreHardwareMonitorLib per-core clocks (the highest); without them the average speed of the cores at work, as in Task Manager: the base clock times "% Processor Performance" of the kernel's *Processor Information* counters (PerfLib V2, `Services/CpuSpeed.cs`); last fallback `CallNtPowerInformation`, which on recent Intel CPUs is the base clock | per-core clocks: **PawnIO + admin**; average speed: nothing |
| CPU temperature | LibreHardwareMonitorLib via MSR | **PawnIO + admin** |
| CPU package power | LibreHardwareMonitorLib via MSR | **PawnIO + admin** |
| RAM (all metrics) | `GlobalMemoryStatusEx` + `GetPerformanceInfo` (committed memory); `NtQuerySystemInformation` for the memory lists (in use, modified, standby, free) and the page file usage | nothing |
| GPU usage / temp / clock / VRAM / power (NVIDIA) | NVML (`nvml.dll` of the NVIDIA driver), read directly by Pulse; LibreHardwareMonitorLib as the fallback | NVIDIA driver (anyone has it) |
| GPU usage / temp / clock / VRAM (AMD, Intel) | LibreHardwareMonitorLib (via ADL for AMD) | AMD or Intel driver (anyone has it) |
| Integrated GPU (no graphics card): usage, memory | LibreHardwareMonitorLib's D3D sensors: the busiest engine (3D, copy, video...), as in Task Manager, and the memory shared with the CPU; no temperature or clocks | Intel driver (anyone has it) |
| GPU power (W), AMD and Intel | LibreHardwareMonitorLib | AMD/Intel driver; an Intel integrated GPU: **PawnIO + admin** |
| Disk I/O total | `NtQuerySystemInformation` (system-wide I/O byte counters) | nothing |
| Disk per-drive free space | `DriveInfo.GetDrives` | nothing |
| SSD temperature | LibreHardwareMonitorLib via SMART | **off by default**, see [Storage](#storage-ssd-temperature) |
| Network I/O | `GetIfEntry2` on the physical adapters that are up (Windows' *HardwareInterface* flag), because the same traffic shows up again on the WFP and QoS filter interfaces and on virtual adapters; if no adapter has the flag, every up interface that is not a filter (loopback and tunnels excluded). The adapters are chosen with `GetIfTable2` every 10 s and when one goes down | nothing |
| Ping, IPs, Wi-Fi SSID | `Ping` to 8.8.8.8, `NetworkInterface`, `netsh wlan` (only when the active connection is Wi-Fi, every 30 s), api.ipify.org for the public IP | Internet for ping and public IP |

Disk I/O comes from the system-wide I/O byte counters: they count every read and write request, file cache hits included, not only what reaches the physical disks.

## The MSR problem

CPU temperature, package power, voltage, and per-core clocks live in **Model-Specific Registers (MSRs)** — special CPU registers only readable from Ring 0 (kernel mode). User-mode code, even running as admin, cannot read them directly.

Tools like HWiNFO64, AIDA64 and CPU-Z bridge this gap with a **signed kernel driver** of their own that exposes MSR reads via IOCTL.

On modern Windows (10/11) with security features like:

- **Driver Signature Enforcement (DSE)** — requires drivers to be signed by Microsoft
- **VBS (Virtualization-Based Security)** — runs the kernel in a sandboxed VM
- **HVCI (Hypervisor-protected Code Integrity)** — extra integrity checks
- **Microsoft Driver Block List** — blocks known vulnerable drivers (WinRing0, the classic one, is on it)

...many older Ring-0 drivers fail to load. WinRing0 is the one LibreHardwareMonitor used to load by itself; version 0.9.6, the one Pulse bundles, has no driver of its own and reads the MSRs (and AMD's SMU) only through **PawnIO**, a signed driver that has to be installed separately. Without PawnIO, running Pulse as admin isn't enough: there is no driver to talk to.

## How to enable the CPU MSR sensors

The same on Intel and AMD:

1. **Install [PawnIO](https://pawnio.eu)**, once: from its site or, in an administrator terminal, `winget install namazso.PawnIO`. It stays installed as a driver service; nothing has to be kept open.
2. **Run Pulse as administrator**: the installer's *Start with administrator privileges* does it at every start with no UAC prompt (see `INSTALLER.md`), or *Restart as administrator* in the tray menu.

**Verify it's installed** (the key LibreHardwareMonitor and Pulse look for):
```powershell
Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO'
# True
```

**Without PawnIO or without the rights**: CPU usage %, the per-core loads and the clock work (the clock is then the average speed of the cores at work, as in Task Manager), CPU temperature and power show `n/a`, and the tooltip on `n/a` says which of the two is missing.

## CPU-family-specific notes

### AMD Ryzen 9 7950X (Zen 4) — reference test platform

Sensors read through LibreHardwareMonitorLib (verified on the reference PC):
- Temperature: `Core (Tctl/Tdie)`, `CCD1 (Tdie)`, `CCD2 (Tdie)`, `Package`
- Power: `Package`, `Core #N (SMU)` per core
- Clock: per-core, up to ~5.7 GHz boost
- Voltage: per-core VID

### Intel i9-11900K (Rocket Lake) with Intel UHD Graphics 750, no graphics card — second test platform

Checked on 12/10/2026 (Windows 11 26200 in Italian, VBS on without HVCI), as administrator and not. LibreHardwareMonitorLib 0.9.6 lists these sensors, all empty without PawnIO:
- Temperature: `CPU Package`, `Core Max`, `Core Average`, `CPU Core #1`–`#8` and their `Distance to TjMax`
- Power: `CPU Package`, `CPU Cores`, `CPU Memory`, `CPU Platform` (0 without PawnIO, not empty)
- Clock: `CPU Core #1`–`#8`

The loads (`CPU Total`, per thread) are there anyway. Without the core clocks Pulse shows the average speed of the cores (`Services/CpuSpeed.cs`): 2.5–3.0 GHz at rest, 4.6 GHz with eight busy threads, the same values as Task Manager's counters, where `CallNtPowerInformation` read 3.50 GHz, the base clock, at all times.

The UHD Graphics 750 has only D3D sensors: the load of each engine (`D3D 3D`, `D3D Copy`, `D3D Video Decode`...), `D3D Shared Memory Used/Free/Total` (the RAM it may take, half of the 32 GB) and `GPU Power`, which needs PawnIO; no temperature, clocks or memory of its own.

### Intel Core gen ≥ 12 (Alder Lake / Raptor Lake)

P-cores and E-cores exposed separately by LHM. Same driver as gen 11: PawnIO.

## GPU support

| Vendor | API used by LHM | Works without admin |
|---|---|---|
| NVIDIA | NVAPI (`nvapi64.dll` in driver folder) | ✅ Yes (user-mode API) |
| AMD discrete | ADL (`atiadlxx.dll` in driver folder) | ✅ Yes |
| AMD integrated (APU) | ADL + WDDM perf counters | ✅ Yes |
| Intel iGPU | D3D statistics of the adapter: load per engine, shared memory | ✅ Yes (its power: PawnIO + admin) |

GPU power, temperature, VRAM total/used, clocks — all work without admin on every vendor, except on an Intel integrated GPU, which has no temperature, clocks or memory of its own and whose power needs PawnIO.

**Multi-GPU caveat**: when you have both a discrete GPU and an iGPU (e.g., RTX 5070 Ti + AMD Radeon Graphics on a Ryzen), LHM may report "GPU Memory Total" of 32+ GB on the iGPU (it's the *shared* memory pool, not real dedicated VRAM). Pulse ignores GPUs with an implausible total VRAM (> 48 GB cap) and takes its readings from the one with the most VRAM, normally the discrete one.

**Integrated GPU only**: a GPU with no memory of its own (or the one LHM calls `gpu-intel-integrated`) is shown only when no card has dedicated memory. Its load is that of the busiest D3D engine, as Task Manager computes it, its memory the RAM it shares with the CPU ("shared" in the widget), and its name fills the detail line when there are no clocks or power to show. Its power is never added to the PC estimate: it is part of the CPU package.

## Storage (SSD) temperature

Off by default since v1.0.0: `HardwareMonitor` opens LibreHardwareMonitorLib with `IsStorageEnabled = false`, because the storage scan cost ~150 ms at startup plus extra handles. Setting it to `true` brings back the SSD temperature next to DSK, read from the SMART attributes.

## How Pulse degrades gracefully

If a sensor isn't available, Pulse:

1. Falls back where a cheap source exists: when LHM has no per-core clocks the CPU clock is the average speed of the cores at work (the kernel's *Processor Information* counters, as in Task Manager), and `CallNtPowerInformation` only if those counters are missing too
2. Shows nothing rather than wrong data: CPU temperature reads `n/a` (with a tooltip saying what is missing: PawnIO, the rights or both), GPU and SSD temperatures disappear, readings under 5 °C are discarded (we removed the WMI Thermal Zone fallback because it was reporting motherboard temp as "CPU temp")
3. Estimates the CPU watts when LHM has none (about 10 W + 1.5 W per % of CPU, capped at 170 W); the total PC power adds the GPU power (not an integrated GPU's, already in the CPU package) and a 30 W baseline, and is marked with `~` prefix when the CPU part is estimated

## Testing on new hardware

When porting to a new CPU/chipset, run the LibreHardwareMonitor app as administrator, with PawnIO installed: it is built on the same library, so it lists the sensors Pulse can read (ideally take the release matching the `LibreHardwareMonitorLib` version in `ResourceMonitor.csproj`).

Then update the sensor name filters in `HardwareMonitor.Update()` if needed — sensor naming varies (e.g., `CPU Package` vs `Package` vs `Core (Tctl/Tdie)`).

## Roadmap

- [ ] HWiNFO64 shared memory backend (for systems where PawnIO can't be installed)
- [ ] AMD μProf / AthenaSDK direct integration
- [ ] Bundle a Pulse-signed driver (requires Microsoft EV cert — not cheap)
- [ ] Detect at install time that PawnIO is missing and offer to install it
