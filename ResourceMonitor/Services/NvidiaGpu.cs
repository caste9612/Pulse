using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace ResourceMonitor.Services;

/// <summary>Una lettura NVML: carico e memoria ci sono sempre, gli altri valori solo se la scheda li dà.</summary>
public readonly record struct NvidiaReading(
    double LoadPercent, double MemoryTotalMb, double MemoryUsedMb,
    double? TempC, double? CoreClockMhz, double? MemoryClockMhz, double? PowerW, double? TotalPowerW);

/// <summary>
/// Le schede NVIDIA lette direttamente con NVML, la libreria del driver: i sei valori che Pulse mostra in circa 1 ms, invece dei
/// 41 sensori di LibreHardwareMonitor (73 ms, 15 ms di CPU ogni due secondi sulla RTX 5070 Ti di riferimento, 11/10/2026).
/// Pensato per non rompere niente: TryOpen dà null se manca qualcosa (niente driver NVIDIA, NVML vecchia senza le funzioni _v2,
/// nessuna scheda, carico o memoria non leggibili) e Pulse resta su LibreHardwareMonitor come prima. nvml.dll si carica solo da
/// System32 o dalla cartella NVSMI di NVIDIA, mai dalla cartella di Pulse.
/// </summary>
public sealed class NvidiaGpu : IDisposable
{
    private const int Success = 0;
    private const int NVML_TEMPERATURE_GPU = 0, NVML_CLOCK_GRAPHICS = 0, NVML_CLOCK_MEM = 2;
    private const double Mb = 1024d * 1024d;

    private readonly InitFn _shutdown;
    private readonly UtilizationFn _utilization;
    private readonly MemoryFn _memory;
    private readonly TemperatureFn? _temperature;
    private readonly ClockFn? _clock;
    private readonly PowerFn? _power;
    private readonly IntPtr[] _devices;
    private readonly IntPtr _best;
    private bool _disposed;

    /// <summary>Il nome della scheda mostrata (quella con più memoria video, se sono più d'una).</summary>
    public string Name { get; }

    private NvidiaGpu(InitFn shutdown, UtilizationFn utilization, MemoryFn memory, TemperatureFn? temperature, ClockFn? clock,
                      PowerFn? power, IntPtr[] devices, IntPtr best, string name)
    {
        _shutdown = shutdown;
        _utilization = utilization;
        _memory = memory;
        _temperature = temperature;
        _clock = clock;
        _power = power;
        _devices = devices;
        _best = best;
        Name = name;
    }

    public static NvidiaGpu? TryOpen()
    {
        IntPtr library = Load();
        if (library == IntPtr.Zero) return null;   // niente driver NVIDIA: normale, nessuna nota
        InitFn? shutdownOnError = null;
        try
        {
            var init = Export<InitFn>(library, "nvmlInit_v2");
            var shutdown = Export<InitFn>(library, "nvmlShutdown");
            var count = Export<CountFn>(library, "nvmlDeviceGetCount_v2");
            var handle = Export<HandleFn>(library, "nvmlDeviceGetHandleByIndex_v2");
            var utilization = Export<UtilizationFn>(library, "nvmlDeviceGetUtilizationRates");
            var memory = Export<MemoryFn>(library, "nvmlDeviceGetMemoryInfo");
            if (init is null || shutdown is null || count is null || handle is null || utilization is null || memory is null)
            {
                PerfLog.Note("nvml not used: missing functions, staying on LibreHardwareMonitor");
                return null;
            }
            int status = init();
            if (status != Success)
            {
                PerfLog.Note($"nvml not used: nvmlInit error {status}, staying on LibreHardwareMonitor");
                return null;
            }
            shutdownOnError = shutdown;

            // le schede che rispondono a carico e memoria; si mostra quella con più memoria video
            var devices = new List<IntPtr>();
            IntPtr best = IntPtr.Zero;
            double bestMemory = -1;
            if (count(out uint total) == Success)
            {
                for (uint i = 0; i < total; i++)
                {
                    if (handle(i, out IntPtr device) != Success) continue;
                    if (utilization(device, out _) != Success || memory(device, out var info) != Success) continue;
                    devices.Add(device);
                    if (info.Total > bestMemory) { bestMemory = info.Total; best = device; }
                }
            }
            if (best == IntPtr.Zero)
            {
                shutdown();
                return null;
            }

            var nameFn = Export<NameFn>(library, "nvmlDeviceGetName");
            var buffer = new byte[96];
            string name = nameFn is not null && nameFn(best, buffer, (uint)buffer.Length) == Success
                ? System.Text.Encoding.ASCII.GetString(buffer).TrimEnd('\0')
                : "NVIDIA";
            return new NvidiaGpu(shutdown, utilization, memory,
                Export<TemperatureFn>(library, "nvmlDeviceGetTemperature"), Export<ClockFn>(library, "nvmlDeviceGetClockInfo"),
                Export<PowerFn>(library, "nvmlDeviceGetPowerUsage"), devices.ToArray(), best, name);
        }
        catch (Exception ex)
        {
            PerfLog.Note($"nvml not used: {ex.Message}");
            try { shutdownOnError?.Invoke(); } catch { }
            return null;
        }
    }

    /// <summary>Legge la scheda mostrata; false se carico o memoria non rispondono più (chi chiama torna a LibreHardwareMonitor).
    /// Il consumo totale somma tutte le schede NVIDIA, come faceva la somma dei sensori di potenza.</summary>
    public bool TryRead(out NvidiaReading reading)
    {
        reading = default;
        if (_disposed) return false;
        if (_utilization(_best, out var load) != Success || _memory(_best, out var memory) != Success) return false;

        double? temperature = _temperature is not null && _temperature(_best, NVML_TEMPERATURE_GPU, out uint celsius) == Success ? celsius : null;
        double? core = _clock is not null && _clock(_best, NVML_CLOCK_GRAPHICS, out uint coreMhz) == Success ? coreMhz : null;
        double? memoryClock = _clock is not null && _clock(_best, NVML_CLOCK_MEM, out uint memMhz) == Success ? memMhz : null;
        double? power = null, totalPower = null;
        if (_power is not null)
        {
            foreach (var device in _devices)
            {
                if (_power(device, out uint milliwatts) != Success) continue;
                double watts = milliwatts / 1000d;
                totalPower = (totalPower ?? 0) + watts;
                if (device == _best) power = watts;
            }
        }
        reading = new NvidiaReading(load.Gpu, memory.Total / Mb, memory.Used / Mb, temperature, core, memoryClock, power, totalPower);
        return true;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try { _shutdown(); } catch { }
    }

    private static IntPtr Load()
    {
        var candidates = new[]
        {
            Path.Combine(Environment.SystemDirectory, "nvml.dll"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "NVIDIA Corporation", "NVSMI", "nvml.dll")
        };
        foreach (var path in candidates)
        {
            if (File.Exists(path) && NativeLibrary.TryLoad(path, out IntPtr library)) return library;
        }
        return IntPtr.Zero;
    }

    private static T? Export<T>(IntPtr library, string name) where T : Delegate =>
        NativeLibrary.TryGetExport(library, name, out IntPtr address) ? Marshal.GetDelegateForFunctionPointer<T>(address) : null;

    [StructLayout(LayoutKind.Sequential)]
    private struct Utilization { public uint Gpu; public uint Memory; }

    [StructLayout(LayoutKind.Sequential)]
    private struct Memory { public ulong Total; public ulong Free; public ulong Used; }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int InitFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CountFn(out uint count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int HandleFn(uint index, out IntPtr device);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int NameFn(IntPtr device, byte[] name, uint length);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int UtilizationFn(IntPtr device, out Utilization utilization);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int MemoryFn(IntPtr device, out Memory memory);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int TemperatureFn(IntPtr device, int sensor, out uint celsius);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int ClockFn(IntPtr device, int type, out uint mhz);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int PowerFn(IntPtr device, out uint milliwatts);
}
