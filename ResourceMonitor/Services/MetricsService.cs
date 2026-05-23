using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Windows.Threading;

namespace ResourceMonitor.Services;

public sealed class MetricsService : INotifyPropertyChanged, IDisposable
{
    public const int HistoryLength = 60;

    private readonly DispatcherTimer _timer;
    private readonly PerformanceCounter _cpuTotal;
    private readonly List<PerformanceCounter> _cpuCores = new();
    private readonly PerformanceCounter? _diskRead;
    private readonly PerformanceCounter? _diskWrite;
    private readonly List<PerformanceCounter> _netRecv = new();
    private readonly List<PerformanceCounter> _netSent = new();
    private readonly PerformanceCounter? _memCommitted;
    private readonly PerformanceCounter? _memCommitLimit;
    private readonly PerformanceCounter? _memCached;
    private readonly PerformanceCounter? _pageFileUsage;
    private readonly ulong _totalPhysicalRam;
    private readonly ulong _totalDedicatedVram;

    public double Cpu { get; private set; }
    public double[] CpuCores { get; private set; } = Array.Empty<double>();
    public double GpuExternal { get; set; }
    public double VramUsedExternalGb { get; set; }
    public double RamUsedPercent { get; private set; }
    public double RamUsedGb { get; private set; }
    public double RamTotalGb { get; private set; }
    public double RamAvailableGb { get; private set; }
    public double RamCachedGb { get; private set; }
    public double RamCommittedGb { get; private set; }
    public double RamCommitLimitGb { get; private set; }
    public double PageFilePercent { get; private set; }
    public double Gpu { get; private set; }
    public double VramUsedPercent { get; private set; }
    public double VramUsedGb { get; private set; }
    public double VramTotalGb { get; private set; }
    public double DiskReadBytesPerSec { get; private set; }
    public double DiskWriteBytesPerSec { get; private set; }
    public double NetRecvBytesPerSec { get; private set; }
    public double NetSentBytesPerSec { get; private set; }

    public Queue<double> CpuHistory { get; } = new(HistoryLength);
    public Queue<double> RamHistory { get; } = new(HistoryLength);
    public Queue<double> GpuHistory { get; } = new(HistoryLength);
    public Queue<double> DiskHistory { get; } = new(HistoryLength);
    public Queue<double> NetHistory { get; } = new(HistoryLength);

    public event PropertyChangedEventHandler? PropertyChanged;
    public event EventHandler? Updated;

    public MetricsService()
    {
        _cpuTotal = new PerformanceCounter("Processor Information", "% Processor Utility", "_Total", true);
        TryWarmup(_cpuTotal);

        _totalPhysicalRam = GetTotalPhysicalMemoryBytes();
        RamTotalGb = _totalPhysicalRam / 1024d / 1024d / 1024d;

        try
        {
            _memCommitted = new PerformanceCounter("Memory", "Committed Bytes", true);
            _memCommitLimit = new PerformanceCounter("Memory", "Commit Limit", true);
            _memCached = new PerformanceCounter("Memory", "Cache Bytes", true);
            _pageFileUsage = new PerformanceCounter("Paging File", "% Usage", "_Total", true);
            TryWarmup(_memCommitted);
            TryWarmup(_memCommitLimit);
            TryWarmup(_memCached);
            TryWarmup(_pageFileUsage);
        }
        catch { }

        try
        {
            _diskRead = new PerformanceCounter("PhysicalDisk", "Disk Read Bytes/sec", "_Total", true);
            _diskWrite = new PerformanceCounter("PhysicalDisk", "Disk Write Bytes/sec", "_Total", true);
            TryWarmup(_diskRead);
            TryWarmup(_diskWrite);
        }
        catch { }

        try
        {
            var netCat = new PerformanceCounterCategory("Network Interface");
            foreach (var inst in netCat.GetInstanceNames())
            {
                if (inst.Contains("Loopback", StringComparison.OrdinalIgnoreCase)) continue;
                if (inst.Contains("isatap", StringComparison.OrdinalIgnoreCase)) continue;
                var recv = new PerformanceCounter("Network Interface", "Bytes Received/sec", inst, true);
                var sent = new PerformanceCounter("Network Interface", "Bytes Sent/sec", inst, true);
                TryWarmup(recv);
                TryWarmup(sent);
                _netRecv.Add(recv);
                _netSent.Add(sent);
            }
        }
        catch { }

        // GPU % e VRAM used adesso letti da HardwareMonitor (LHM). Niente PerfCounter GPU qui.

        _totalDedicatedVram = GetDedicatedVramBytes();
        VramTotalGb = _totalDedicatedVram / 1024d / 1024d / 1024d;

        for (int i = 0; i < HistoryLength; i++)
        {
            CpuHistory.Enqueue(0);
            RamHistory.Enqueue(0);
            GpuHistory.Enqueue(0);
            DiskHistory.Enqueue(0);
            NetHistory.Enqueue(0);
        }

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => Tick();
    }

    public void Start()
    {
        _timer.Start();
        Tick();
    }
    public void Stop() => _timer.Stop();

    private static void TryWarmup(PerformanceCounter c)
    {
        try { c.NextValue(); } catch { }
    }

    private void Tick()
    {
        var sw = Stopwatch.StartNew();
        try
        {
            Cpu = Math.Clamp(SafeRead(_cpuTotal), 0, 100);

            var memStatus = new MEMORYSTATUSEX();
            memStatus.dwLength = (uint)Marshal.SizeOf(memStatus);
            if (GlobalMemoryStatusEx(ref memStatus))
            {
                ulong used = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
                RamUsedGb = used / 1024d / 1024d / 1024d;
                RamAvailableGb = memStatus.ullAvailPhys / 1024d / 1024d / 1024d;
                RamUsedPercent = memStatus.dwMemoryLoad;
            }

            RamCachedGb = SafeRead(_memCached) / 1024d / 1024d / 1024d;
            RamCommittedGb = SafeRead(_memCommitted) / 1024d / 1024d / 1024d;
            RamCommitLimitGb = SafeRead(_memCommitLimit) / 1024d / 1024d / 1024d;
            PageFilePercent = SafeRead(_pageFileUsage);

            double diskRead = SafeRead(_diskRead);
            double diskWrite = SafeRead(_diskWrite);
            DiskReadBytesPerSec = diskRead;
            DiskWriteBytesPerSec = diskWrite;
            double diskBps = diskRead + diskWrite;

            double netRecv = _netRecv.Sum(SafeRead);
            double netSent = _netSent.Sum(SafeRead);
            NetRecvBytesPerSec = netRecv;
            NetSentBytesPerSec = netSent;
            double netBps = netRecv + netSent;

            Gpu = GpuExternal;
            VramUsedGb = VramUsedExternalGb;

            PushHistory(CpuHistory, Cpu);
            PushHistory(RamHistory, RamUsedPercent);
            PushHistory(GpuHistory, Gpu);
            PushHistory(DiskHistory, diskBps);
            PushHistory(NetHistory, netBps);

            OnChanged(nameof(Cpu));
            OnChanged(nameof(RamUsedPercent));
            OnChanged(nameof(RamUsedGb));
            OnChanged(nameof(Gpu));
            OnChanged(nameof(VramUsedPercent));
            OnChanged(nameof(VramUsedGb));
            OnChanged(nameof(DiskReadBytesPerSec));
            OnChanged(nameof(DiskWriteBytesPerSec));
            OnChanged(nameof(NetRecvBytesPerSec));
            OnChanged(nameof(NetSentBytesPerSec));

            Updated?.Invoke(this, EventArgs.Empty);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MetricsService tick error: {ex.Message}");
        }
        finally
        {
            sw.Stop();
            PerfLog.LogSlow("MetricsTick", sw.ElapsedMilliseconds, 100);
        }
    }

    private static double SafeRead(PerformanceCounter? c)
    {
        if (c is null) return 0;
        try { return c.NextValue(); } catch { return 0; }
    }

    private static void PushHistory(Queue<double> q, double v)
    {
        if (q.Count >= HistoryLength) q.Dequeue();
        q.Enqueue(v);
    }

    private void OnChanged([CallerMemberName] string? name = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));

    public void Dispose()
    {
        _timer.Stop();
        _cpuTotal.Dispose();
        foreach (var c in _cpuCores) c.Dispose();
        _memCommitted?.Dispose();
        _memCommitLimit?.Dispose();
        _memCached?.Dispose();
        _pageFileUsage?.Dispose();
        _diskRead?.Dispose();
        _diskWrite?.Dispose();
        foreach (var c in _netRecv) c.Dispose();
        foreach (var c in _netSent) c.Dispose();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct MEMORYSTATUSEX
    {
        public uint dwLength;
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX lpBuffer);

    private static ulong GetTotalPhysicalMemoryBytes()
    {
        var memStatus = new MEMORYSTATUSEX();
        memStatus.dwLength = (uint)Marshal.SizeOf(memStatus);
        return GlobalMemoryStatusEx(ref memStatus) ? memStatus.ullTotalPhys : 0UL;
    }

    private static ulong GetDedicatedVramBytes()
    {
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT AdapterRAM FROM Win32_VideoController");
            ulong max = 0;
            foreach (var obj in searcher.Get())
            {
                if (obj["AdapterRAM"] is uint v && v > max) max = v;
            }
            return max;
        }
        catch
        {
            return 0;
        }
    }
}
