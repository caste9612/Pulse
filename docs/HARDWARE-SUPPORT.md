# Hardware Support

Which sensors work on which hardware, and why some need extra software.

## TL;DR

| Sensor | Source | Requires |
|---|---|---|
| CPU usage % | Windows PerfCounter | nothing |
| CPU clock GHz | WMI `Win32_Processor.CurrentClockSpeed` | nothing |
| CPU temperature | LibreHardwareMonitorLib via MSR | **signed kernel driver + admin** |
| CPU package power | LibreHardwareMonitorLib via MSR | **signed kernel driver + admin** |
| RAM (all metrics) | `GlobalMemoryStatusEx` + PerfCounter | nothing |
| GPU usage / temp / clock / VRAM (NVIDIA) | LibreHardwareMonitorLib via NVAPI | NVIDIA driver (anyone has it) |
| GPU usage / temp / clock / VRAM (AMD) | LibreHardwareMonitorLib via ADL | AMD driver (anyone has it) |
| GPU power (W) | LibreHardwareMonitorLib | NVIDIA/AMD driver |
| Disk I/O total | PerfCounter `PhysicalDisk` | nothing |
| Disk per-drive free space | `DriveInfo.GetDrives` | nothing |
| SSD temperature | LibreHardwareMonitorLib via SMART | nothing (SMART works without admin) |
| Network I/O | PerfCounter `Network Interface` | nothing |
| Ping, IPs, Wi-Fi SSID | `Ping`, `NetworkInterface`, `netsh` | Internet for public IP |

## The MSR problem

CPU temperature, package power, voltage, and per-core clocks live in **Model-Specific Registers (MSRs)** — special CPU registers only readable from Ring 0 (kernel mode). User-mode code, even running as admin, cannot read them directly.

Tools like HWiNFO64, AIDA64, CPU-Z, and our LibreHardwareMonitor library bridge this gap by installing a **signed kernel driver** that exposes MSR reads via IOCTL.

On modern Windows (10/11) with security features like:

- **Driver Signature Enforcement (DSE)** — requires drivers to be signed by Microsoft
- **VBS (Virtualization-Based Security)** — runs the kernel in a sandboxed VM
- **HVCI (Hypervisor-protected Code Integrity)** — extra integrity checks
- **Microsoft Driver Block List** — blocks known vulnerable drivers (WinRing0, the classic one, is on it)

...many older Ring-0 drivers fail to load. This is why running our app as admin alone isn't enough on some systems: the driver itself is blocked.

## How to enable CPU MSR sensors on each platform

### AMD Ryzen (Zen 1 / 2 / 3 / 4 / 5)

**Install [AMD Ryzen Master](https://www.amd.com/en/products/processors/ryzen-master.html)** (free, signed by AMD). It ships a signed kernel driver `AMDRyzenMasterDriverV31.sys` that:

- Is signed by AMD (passes DSE)
- Is not on the Microsoft block list
- Installs as a Windows service set to Automatic startup → loads at boot, stays loaded
- Provides MSR access for any process that can open its IOCTL interface

LibreHardwareMonitorLib in our app detects this driver and uses it. **You don't need to keep Ryzen Master GUI open** — just installed once is enough.

**Verify it's loaded**:
```powershell
Get-Service AMDRyzenMasterDriverV31
# StartType: Automatic, Status: Running
```

### Intel Core (8th-14th Gen, including the i9-11900K)

Two paths:

**A. LHM's own driver** (works if VBS is off)
- Run Pulse once as admin → LHM tries to install its own driver
- If VBS is off: works
- If VBS is on: driver may be blocked → fallback needed

**B. HWiNFO64 driver** (works under VBS)
- Install [HWiNFO64](https://www.hwinfo.com) (free for personal use)
- It bundles a driver signed via newer Microsoft attestation, passes VBS
- Configure HWiNFO64 to run as a startup service (it has the option in Settings → "Run Sensors-only mode" + auto-start)
- Enable "Shared Memory Support" in HWiNFO64 settings
- *Future:* Pulse will read from HWiNFO64's shared memory (not yet implemented — open PR welcome)

**C. Intel Power Gadget** *(deprecated, but old versions still work on 11th gen)*
- The driver `EnergyDrv` was Microsoft-signed and exposed Intel RAPL
- Officially discontinued by Intel in 2023 but old installer (3.6+) still installs and works
- LibreHardwareMonitorLib detects and uses it if present

**On Intel without any of these**: CPU usage % and clock work (via WMI), CPU temperature/power show `n/a`.

### Disable VBS entirely (last resort, not recommended)

```powershell
# Admin PowerShell:
bcdedit /set hypervisorlaunchtype off
# Reboot
```

After reboot, VBS is off and LHM's own driver works on most systems. **But you lose** Microsoft Defender Application Guard, Credential Guard, Hyper-V, WSL2, and other kernel-level protections. Only do this on dev/gaming rigs you don't use for sensitive work.

To re-enable: `bcdedit /set hypervisorlaunchtype auto` + reboot.

## CPU-family-specific notes

### AMD Ryzen 9 7950X (Zen 4) — reference test platform

Sensors read via Ryzen Master driver (verified):
- Temperature: `Core (Tctl/Tdie)`, `CCD1 (Tdie)`, `CCD2 (Tdie)`, `Package`
- Power: `Package`, `Core #N (SMU)` per core
- Clock: per-core, up to ~5.7 GHz boost
- Voltage: per-core VID

### Intel i9-11900K (Rocket Lake) — target Intel platform

LibreHardwareMonitorLib exposes (when driver loads):
- Temperature: `CPU Package`, `CPU Core #N` (per-core DTS)
- Power: `CPU Package`, `IA Cores`, `Uncore`, `DRAM`
- Clock: `Bus Speed`, per-core `CPU Core #N` (effective)

If LHM's own driver fails under VBS, install HWiNFO64 (path B above) or Intel Power Gadget (path C).

### Intel Core gen ≥ 12 (Alder Lake / Raptor Lake)

P-cores and E-cores exposed separately by LHM. Same driver options as gen 11.

## GPU support

| Vendor | API used by LHM | Works without admin |
|---|---|---|
| NVIDIA | NVAPI (`nvapi64.dll` in driver folder) | ✅ Yes (user-mode API) |
| AMD discrete | ADL (`atiadlxx.dll` in driver folder) | ✅ Yes |
| AMD integrated (APU) | ADL + WDDM perf counters | ✅ Yes |
| Intel iGPU | Intel Performance Counters via DXGI | ✅ Yes |

GPU power, temperature, VRAM total/used, clocks — all work without admin on every vendor.

**Multi-GPU caveat**: when you have both a discrete GPU and an iGPU (e.g., RTX 5070 Ti + AMD Radeon Graphics on a Ryzen), LHM may report "GPU Memory Total" of 32+ GB on the iGPU (it's the *shared* memory pool, not real dedicated VRAM). Pulse filters out GPUs with implausible total VRAM (> 48 GB cap) and prefers the discrete one.

## Storage (SSD) temperature

Read from SMART attributes via LibreHardwareMonitorLib. Works without admin — Windows lets non-elevated processes query SMART for read-only attributes.

## How Pulse degrades gracefully

If a sensor isn't available, Pulse:

1. Tries WMI/PerfCounter fallback where one exists (clock via `Win32_Processor`)
2. Hides the field rather than showing wrong data (we removed the WMI Thermal Zone fallback because it was reporting motherboard temp as "CPU temp")
3. Estimates total PC power from CPU% × TDP heuristic when actual CPU watts aren't available, marked with `~` prefix

## Testing on new hardware

When porting to a new CPU/chipset, enable the diagnostic sensor dump in `HardwareMonitor.Update()` by uncommenting the `DumpSensors()` call, then check `%TEMP%\pulse-sensors.txt` to see what sensors LHM exposes.

Then update the sensor name filters in `HardwareMonitor.cs` if needed — sensor naming varies (e.g., `CPU Package` vs `Package` vs `Tctl/Tdie`).

## Roadmap

- [ ] HWiNFO64 shared memory backend (for Intel under VBS)
- [ ] AMD μProf / AthenaSDK direct integration (alternative to Ryzen Master)
- [ ] Bundle a Pulse-signed driver (requires Microsoft EV cert — not cheap)
- [ ] Detect platform at install time and prompt to install AMD Ryzen Master / HWiNFO64
