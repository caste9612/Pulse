using System;
using System.Diagnostics;
using System.Linq;
using LibreHardwareMonitor.Hardware;

namespace ResourceMonitor.Services;

public sealed class HardwareMonitor : IDisposable
{
    private Computer? _computer;
    private bool _enabled;
    private volatile bool _paused;
    private readonly Task _initTask;

    // la scheda NVIDIA letta con NVML (NvidiaGpu): la prima lettura completa di LibreHardwareMonitor sceglie la scheda da mostrare
    // come sempre (la memoria video più grande); se è una NVIDIA che NVML vede, da lì in poi le GPU le legge NVML. A ogni errore
    // si torna a LibreHardwareMonitor. PULSE_NO_NVML=1 la spegne
    private NvidiaGpu? _nvidia;
    private bool _nvidiaActive;
    private bool _gpuChosen;
    // senza NVML, dopo la prima lettura LibreHardwareMonitor aggiorna solo la scheda mostrata: le altre (la GPU integrata accanto a
    // una dedicata) costavano tempo e mettevano nel consumo totale watt che sono già nel pacchetto della CPU. Null = tutte
    private IHardware? _chosenGpu;

    // la frequenza dei core LibreHardwareMonitor la legge dai registri MSR, cioè da amministratore e con il driver PawnIO; senza,
    // la velocità media come in Gestione attività (CpuSpeed)
    private CpuSpeed? _speed;
    private bool _speedTried;

    public double? CpuTempC { get; private set; }
    public double? CpuClockMhz { get; private set; }
    /// <summary><see cref="CpuClockMhz"/> è la velocità media dei core al lavoro (CpuSpeed), non la frequenza del core più veloce.</summary>
    public bool CpuClockIsAverage { get; private set; }
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
    /// <summary>La GPU mostrata è integrata nel processore: il suo consumo è già in quello del pacchetto della CPU.</summary>
    public bool GpuIntegrated { get; private set; }
    /// <summary><see cref="GpuMemTotalMb"/> e <see cref="GpuMemUsedMb"/> sono la RAM condivisa con la CPU, non memoria video
    /// dedicata: una GPU integrata non ne ha.</summary>
    public bool GpuMemShared { get; private set; }
    public double? GpuLoadPercent { get; private set; }
    public double? StorageTempC { get; private set; }
    public string? GpuName { get; private set; }
    public string? CpuName { get; private set; }

    /// <summary>Pulse gira da amministratore: LibreHardwareMonitor può leggere i registri MSR, se c'è il driver PawnIO.</summary>
    public static bool IsElevated { get; } = CheckElevated();

    /// <summary>Il driver PawnIO, con cui LibreHardwareMonitor 0.9.6 legge i registri MSR (temperatura, consumo e frequenze della
    /// CPU, Intel e AMD): la libreria non ne porta uno suo e va installato a parte (pawnio.eu, winget namazso.PawnIO). Lo si cerca
    /// dove lo cerca la libreria, nella chiave di disinstallazione.</summary>
    public static bool PawnIoInstalled { get; } = CheckPawnIo();

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
            if (Environment.GetEnvironmentVariable("PULSE_NO_NVML") != "1") _nvidia = NvidiaGpu.TryOpen();
            // senza diritti o senza PawnIO le frequenze dei core non arriveranno: la velocità media si apre subito, così c'è già
            // alla prima lettura
            if (!IsElevated || !PawnIoInstalled) OpenSpeed();
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

    private void OpenSpeed()
    {
        _speedTried = true;
        _speed = CpuSpeed.TryOpen();
    }

    /// <summary>Smette di leggere i sensori, per sempre: il driver video è cambiato e una chiamata a NVML col driver vecchio manda
    /// in crash il processo (10/10/2026). Pulse poi riparte da capo (App.RestartAfterGpuChange).</summary>
    public void Pause() => _paused = true;

    public void Update()
    {
        try { _initTask.Wait(5000); } catch { }
        if (_paused || !_enabled || _computer is null) return;
        try
        {
            double? cpuTemp = null, cpuClock = null;
            double? gpuTemp = null, gpuClock = null, gpuMemClock = null, gpuPower = null, gpuLoad = null;
            double? storageTemp = null;
            double? bestGpuMemTotal = null, bestGpuMemUsed = null;
            bool gpuIntegrated = false, gpuShared = false;
            double totalGpuPower = 0;
            int gpuPowerCount = 0;
            IHardware? bestGpu = null;
            GpuReading? best = null;
            bool nvml = _nvidiaActive;

            foreach (var hw in _computer.Hardware)
            {
                if (_paused) return;   // il driver video è cambiato a metà giro
                bool gpu = hw.HardwareType is HardwareType.GpuNvidia or HardwareType.GpuAmd or HardwareType.GpuIntel;
                if (gpu && (nvml || (_chosenGpu is not null && hw != _chosenGpu)))
                    continue;          // la scheda mostrata la legge NVML (sotto), le altre non si leggono più
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
                else if (gpu)
                {
                    // la scheda da mostrare: quella con più memoria video dedicata plausibile; un'integrata (punteggio 0) se
                    // non c'è altro. A parità vince la prima
                    var reading = ReadGpu(hw);
                    if (best is null || reading.Score > best.Value.Score) { best = reading; bestGpu = hw; }
                    // il consumo di un'integrata è già nel pacchetto della CPU: sommarlo gonfiava la stima del PC (53 W a riposo
                    // della Radeon integrata del 7950X di riferimento)
                    if (!reading.Integrated && reading.PowerSum > 0) { totalGpuPower += reading.PowerSum; gpuPowerCount++; }
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

            if (best is GpuReading shown)
            {
                gpuTemp = shown.Temp;
                gpuClock = shown.Clock;
                gpuMemClock = shown.MemClock;
                gpuPower = shown.Power;
                gpuLoad = shown.Load;
                bestGpuMemTotal = shown.MemTotal;
                bestGpuMemUsed = shown.MemUsed;
                gpuIntegrated = shown.Integrated;
                gpuShared = shown.Shared;
            }

            if (nvml)
            {
                if (_nvidia!.TryRead(out var r))
                {
                    gpuTemp = r.TempC;
                    gpuClock = r.CoreClockMhz;
                    gpuMemClock = r.MemoryClockMhz;
                    gpuPower = r.PowerW;
                    bestGpuMemTotal = r.MemoryTotalMb;
                    bestGpuMemUsed = r.MemoryUsedMb;
                    gpuLoad = r.LoadPercent;
                    if (r.TotalPowerW is double watts) { totalGpuPower = watts; gpuPowerCount = 1; }
                }
                else
                {
                    // NVML non risponde più: da qui LibreHardwareMonitor, come prima; questo giro tiene i valori di prima
                    PerfLog.Note("nvml read failed: back to LibreHardwareMonitor");
                    _nvidiaActive = false;
                    _nvidia.Dispose();
                    _nvidia = null;
                    return;
                }
            }
            else if (!_gpuChosen)
            {
                // la prima lettura completa ha scelto la scheda da mostrare: se è una NVIDIA e NVML c'è, da qui la legge NVML;
                // altrimenti LibreHardwareMonitor legge solo quella (anche un'integrata, se è l'unica)
                _gpuChosen = true;
                _chosenGpu = bestGpu;
                if (bestGpu is not null) GpuName = bestGpu.Name;
                if (_nvidia is not null && bestGpu?.HardwareType == HardwareType.GpuNvidia) _nvidiaActive = true;
                else { _nvidia?.Dispose(); _nvidia = null; }
            }

            // sotto i 5 °C non è una lettura vera; la Thermal Zone di WMI, che dava la scheda madre come CPU, non si usa più
            if (cpuTemp is not null && cpuTemp < 5) cpuTemp = null;

            // la frequenza del core più veloce da LibreHardwareMonitor; senza, la velocità media dei core al lavoro. Aperta qui
            // (Pulse elevato con PawnIO, ma la CPU non dà le frequenze) il primo campione è adesso e il valore arriva al giro dopo
            double? average = null;
            if (cpuClock is null)
            {
                if (!_speedTried) OpenSpeed();
                else average = _speed?.ReadMhz();
            }

            CpuTempC = cpuTemp;
            CpuClockMhz = cpuClock ?? average;
            CpuClockIsAverage = cpuClock is null && average is not null;
            GpuTempC = gpuTemp;
            GpuClockMhz = gpuClock;
            GpuMemClockMhz = gpuMemClock;
            GpuPowerW = gpuPower;
            GpuMemTotalMb = bestGpuMemTotal;
            GpuMemUsedMb = bestGpuMemUsed;
            GpuIntegrated = !nvml && gpuIntegrated;
            GpuMemShared = !nvml && gpuShared;
            GpuLoadPercent = gpuLoad ?? GpuLoadPercent;
            TotalGpuPowerW = gpuPowerCount > 0 ? totalGpuPower : null;
            StorageTempC = storageTemp;
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"HardwareMonitor update failed: {ex.Message}");
        }
    }

    // una scheda video letta da LibreHardwareMonitor; Score dice quanto è adatta a essere quella mostrata
    private readonly record struct GpuReading(
        double Score, bool Integrated, bool Shared, double? Load, double? MemTotal, double? MemUsed,
        double? Temp, double? Clock, double? MemClock, double? Power, double PowerSum);

    // Il carico è "GPU Core" se c'è; la GPU integrata Intel ha solo quello dei motori D3D (3D, copia, video...) e, come Gestione
    // attività, si prende il più carico. La memoria è quella dedicata, del produttore ("GPU Memory") o vista da D3D, plausibile
    // fino a 48 GB; un'integrata non ne ha e usa la RAM, e si mostra quella condivisa. Integrata è una scheda senza memoria
    // dedicata che LibreHardwareMonitor chiama così ("/gpu-intel-integrated/") o che ha solo memoria condivisa
    private static GpuReading ReadGpu(IHardware hw)
    {
        double? coreLoad = null, engineLoad = null, temp = null, clock = null, memClock = null, power = null;
        double? memTotal = null, memUsed = null, d3dTotal = null, d3dUsed = null, sharedTotal = null, sharedUsed = null;
        double powerSum = 0;
        foreach (var s in hw.Sensors)
        {
            if (s.Value is not float value) continue;
            switch (s.SensorType)
            {
                case SensorType.Temperature:
                    temp ??= value;
                    break;
                case SensorType.Clock:
                    if (s.Name.Contains("Core", StringComparison.OrdinalIgnoreCase)) clock ??= value;
                    if (s.Name.Contains("Memory", StringComparison.OrdinalIgnoreCase)) memClock ??= value;
                    break;
                case SensorType.Power when s.Name.Contains("GPU", StringComparison.OrdinalIgnoreCase):
                    // il valore principale per la UI: "GPU Package" (NVIDIA), "GPU Core", "GPU Power" (Intel)
                    if (Is(s, "GPU Package") || Is(s, "GPU Core") || Is(s, "GPU Power")) power ??= value;
                    powerSum += value;
                    break;
                case SensorType.Load:
                    if (Is(s, "GPU Core")) coreLoad ??= value;
                    else if (s.Name.StartsWith("D3D", StringComparison.OrdinalIgnoreCase)) engineLoad = Math.Max(engineLoad ?? 0, value);
                    break;
                case SensorType.SmallData:
                    if (Is(s, "GPU Memory Total")) memTotal ??= value;
                    else if (Is(s, "GPU Memory Used")) memUsed ??= value;
                    else if (Is(s, "D3D Dedicated Memory Total")) d3dTotal ??= value;
                    else if (Is(s, "D3D Dedicated Memory Used")) d3dUsed ??= value;
                    else if (Is(s, "D3D Shared Memory Total")) sharedTotal ??= value;
                    else if (Is(s, "D3D Shared Memory Used")) sharedUsed ??= value;
                    break;
            }
        }

        // per una GPU che LibreHardwareMonitor dice integrata, la memoria "dedicata" (se la riporta) è la piccola parte di RAM
        // riservata all'avvio: conta come memoria video solo da 1 GB in su, cioè su una scheda vera
        bool integratedId = hw.Identifier.ToString().Contains("integrated", StringComparison.OrdinalIgnoreCase);
        double? total = memTotal ?? d3dTotal;
        double? used = memTotal is not null ? memUsed : d3dUsed;
        bool dedicated = total is double t && t > 0 && t <= 48d * 1024 && (!integratedId || t >= 1024);
        bool shared = !dedicated && sharedTotal is double st && st > 0;
        return new GpuReading(
            Score: dedicated ? total!.Value : 0,
            Integrated: !dedicated && (integratedId || shared),
            Shared: shared,
            Load: coreLoad ?? engineLoad,
            MemTotal: dedicated ? total : shared ? sharedTotal : null,
            MemUsed: dedicated ? used : shared ? sharedUsed : null,
            Temp: temp, Clock: clock, MemClock: memClock, Power: power, PowerSum: powerSum);
    }

    private static bool Is(ISensor sensor, string name) => sensor.Name.Equals(name, StringComparison.OrdinalIgnoreCase);

    private static bool CheckElevated()
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            return new System.Security.Principal.WindowsPrincipal(identity).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    private static bool CheckPawnIo()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO");
            return key is not null;
        }
        catch { return false; }
    }

    public void Dispose()
    {
        _nvidia?.Dispose();
        _speed?.Dispose();
        try { _computer?.Close(); } catch { }
    }
}
