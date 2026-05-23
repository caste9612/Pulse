using System;
using System.Diagnostics;
using System.Linq;
using LibreHardwareMonitor.Hardware;

namespace ResourceMonitor.Services;

public sealed class HardwareMonitor : IDisposable
{
    private Computer? _computer;
    private bool _enabled;
    private readonly Task _initTask;

    public double? CpuTempC { get; private set; }
    public double? CpuClockMhz { get; private set; }
    public double? CpuPowerW { get; private set; }
    public double? CpuVoltageV { get; private set; }
    public double? CpuLoadPercent { get; private set; }
    public double? GpuTempC { get; private set; }
    public double? GpuClockMhz { get; private set; }
    public double? GpuMemClockMhz { get; private set; }
    public double? GpuPowerW { get; private set; }
    public double? TotalGpuPowerW { get; private set; }
    public double? GpuMemTotalMb { get; private set; }
    public double? GpuMemUsedMb { get; private set; }
    public double? GpuLoadPercent { get; private set; }
    public double? StorageTempC { get; private set; }
    public string? GpuName { get; private set; }
    public string? CpuName { get; private set; }
    public bool CpuTempFromWmi { get; private set; }

    public HardwareMonitor()
    {
        // TEST: skip init to measure LHM cost
        if (Environment.GetEnvironmentVariable("PULSE_NO_LHM") == "1")
        {
            _initTask = System.Threading.Tasks.Task.CompletedTask;
            _enabled = false;
            return;
        }
        _initTask = System.Threading.Tasks.Task.Run(InitializeNow);
    }

    private void InitializeNow()
    {
        try
        {
            _computer = new Computer
            {
                IsCpuEnabled = true,
                IsGpuEnabled = true,
                IsStorageEnabled = false,
                IsMemoryEnabled = false,
                IsMotherboardEnabled = false,
                IsNetworkEnabled = false,
                IsControllerEnabled = false
            };
            _computer.Open();
            _enabled = true;
            CpuName = _computer.Hardware.FirstOrDefault(h => h.HardwareType == HardwareType.Cpu)?.Name;
            GpuName = _computer.Hardware.FirstOrDefault(h =>
                h.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd or HardwareType.GpuIntel)?.Name;
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"HardwareMonitor init failed: {ex.Message}");
            _enabled = false;
        }
    }

    public void Update()
    {
        try { _initTask.Wait(5000); } catch { }
        if (!_enabled || _computer is null) return;
        try
        {
            double? cpuTemp = null, cpuClock = null;
            double? gpuTemp = null, gpuClock = null, gpuMemClock = null, gpuPower = null;
            double? storageTemp = null;
            double? bestGpuMemTotal = null, bestGpuMemUsed = null;
            double totalGpuPower = 0;
            int gpuPowerCount = 0;

            foreach (var hw in _computer.Hardware)
            {
                hw.Update();

                if (hw.HardwareType == HardwareType.Cpu)
                {
                    double? cpuPower = null;
                    double? cpuVoltage = null;
                    double? cpuLoad = null;
                    foreach (var s in hw.Sensors)
                    {
                        if (s.Value is null) continue;
                        if (s.SensorType == SensorType.Temperature &&
                            (s.Name.Contains("Package", StringComparison.OrdinalIgnoreCase) ||
                             s.Name.Contains("Core (Tctl/Tdie)", StringComparison.OrdinalIgnoreCase) ||
                             s.Name.Contains("CPU Package", StringComparison.OrdinalIgnoreCase)))
                        {
                            cpuTemp = s.Value;
                        }
                        else if (s.SensorType == SensorType.Temperature && cpuTemp is null)
                        {
                            cpuTemp = s.Value;
                        }
                        if (s.SensorType == SensorType.Clock &&
                            s.Name.StartsWith("CPU Core", StringComparison.OrdinalIgnoreCase) &&
                            (cpuClock is null || s.Value > cpuClock))
                        {
                            cpuClock = s.Value;
                        }
                        if (s.SensorType == SensorType.Power &&
                            (s.Name.Equals("Package", StringComparison.OrdinalIgnoreCase) ||
                             s.Name.Equals("CPU Package", StringComparison.OrdinalIgnoreCase) ||
                             s.Name.Contains("Package Power", StringComparison.OrdinalIgnoreCase)) &&
                            cpuPower is null)
                        {
                            cpuPower = s.Value;
                        }
                        else if (s.SensorType == SensorType.Power && cpuPower is null)
                        {
                            cpuPower = s.Value;
                        }
                        if (s.SensorType == SensorType.Voltage &&
                            s.Name.Contains("Core", StringComparison.OrdinalIgnoreCase) && cpuVoltage is null)
                        {
                            cpuVoltage = s.Value;
                        }
                        if (s.SensorType == SensorType.Load &&
                            s.Name.Equals("CPU Total", StringComparison.OrdinalIgnoreCase) && cpuLoad is null)
                        {
                            cpuLoad = s.Value;
                        }
                    }
                    CpuPowerW = cpuPower;
                    CpuVoltageV = cpuVoltage;
                    CpuLoadPercent = cpuLoad;
                }
                else if (hw.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd or HardwareType.GpuIntel)
                {
                    double? gpuMemTotal = null, gpuMemUsed = null, gpuLoad = null;
                    double? localTemp = null, localClock = null, localMemClock = null, localPower = null;
                    double localGpuPowerSum = 0;
                    foreach (var s in hw.Sensors)
                    {
                        if (s.Value is null) continue;
                        if (s.SensorType == SensorType.Temperature && localTemp is null) localTemp = s.Value;
                        if (s.SensorType == SensorType.Clock)
                        {
                            if (s.Name.Contains("Core", StringComparison.OrdinalIgnoreCase) && localClock is null)
                                localClock = s.Value;
                            if (s.Name.Contains("Memory", StringComparison.OrdinalIgnoreCase) && localMemClock is null)
                                localMemClock = s.Value;
                        }
                        if (s.SensorType == SensorType.Power &&
                            s.Name.Contains("GPU", StringComparison.OrdinalIgnoreCase))
                        {
                            // localPower = "primary" reading per UI (Package o Core)
                            if (localPower is null &&
                                (s.Name.Equals("GPU Package", StringComparison.OrdinalIgnoreCase) ||
                                 s.Name.Equals("GPU Core", StringComparison.OrdinalIgnoreCase)))
                            {
                                localPower = s.Value;
                            }
                            localGpuPowerSum += s.Value.Value;
                        }
                        if (s.SensorType == SensorType.Load &&
                            s.Name.Equals("GPU Core", StringComparison.OrdinalIgnoreCase) && gpuLoad is null)
                        {
                            gpuLoad = s.Value;
                        }
                        if (s.SensorType == SensorType.SmallData)
                        {
                            if (s.Name.Equals("GPU Memory Total", StringComparison.OrdinalIgnoreCase) && gpuMemTotal is null)
                                gpuMemTotal = s.Value;
                            else if (s.Name.Equals("GPU Memory Used", StringComparison.OrdinalIgnoreCase) && gpuMemUsed is null)
                                gpuMemUsed = s.Value;
                        }
                    }

                    bool plausibleTotal = gpuMemTotal is double tg && tg > 0 && tg <= 48d * 1024;
                    bool better = bestGpuMemTotal is null
                        || (plausibleTotal && (bestGpuMemTotal is null || gpuMemTotal > bestGpuMemTotal && bestGpuMemTotal > 48 * 1024));

                    if (plausibleTotal && (bestGpuMemTotal is null || gpuMemTotal > bestGpuMemTotal))
                    {
                        bestGpuMemTotal = gpuMemTotal;
                        bestGpuMemUsed = gpuMemUsed;
                        gpuTemp = localTemp ?? gpuTemp;
                        gpuClock = localClock ?? gpuClock;
                        gpuMemClock = localMemClock ?? gpuMemClock;
                        gpuPower = localPower ?? gpuPower;
                        GpuLoadPercent = gpuLoad ?? GpuLoadPercent;
                    }
                    else if (bestGpuMemTotal is null)
                    {
                        gpuTemp ??= localTemp;
                        gpuClock ??= localClock;
                        gpuMemClock ??= localMemClock;
                        gpuPower ??= localPower;
                        GpuLoadPercent ??= gpuLoad;
                    }
                    if (localGpuPowerSum > 0) { totalGpuPower += localGpuPowerSum; gpuPowerCount++; }
                }
                else if (hw.HardwareType == HardwareType.Storage)
                {
                    foreach (var s in hw.Sensors)
                    {
                        if (s.Value is null) continue;
                        if (s.SensorType == SensorType.Temperature && (storageTemp is null || s.Value > storageTemp))
                        {
                            storageTemp = s.Value;
                        }
                    }
                }
            }

            // CPU temp da WMI Thermal Zone è motherboard/chipset, NON la CPU.
            // Mostriamo solo se davvero da LHM (MSR), altrimenti n/a.
            if (cpuTemp is not null && cpuTemp < 5) cpuTemp = null;
            CpuTempFromWmi = false;


            CpuTempC = cpuTemp;
            CpuClockMhz = cpuClock;
            GpuTempC = gpuTemp;
            GpuClockMhz = gpuClock;
            GpuMemClockMhz = gpuMemClock;
            GpuPowerW = gpuPower;
            GpuMemTotalMb = bestGpuMemTotal;
            GpuMemUsedMb = bestGpuMemUsed;
            TotalGpuPowerW = gpuPowerCount > 0 ? totalGpuPower : null;
            StorageTempC = storageTemp;
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"HardwareMonitor update failed: {ex.Message}");
        }
    }

    public void Dispose()
    {
        try { _computer?.Close(); } catch { }
    }
}
