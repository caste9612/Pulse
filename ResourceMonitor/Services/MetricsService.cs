using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Windows.Threading;

namespace ResourceMonitor.Services;

public sealed class MetricsService : INotifyPropertyChanged, IDisposable
{
    public const int HistoryLength = 60;

    private readonly DispatcherTimer _timer;
    private readonly NativeMetrics _native = new();

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
    public double CpuFreqMhz { get; private set; }
    public double Gpu { get; private set; }
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
        // Init: pre-fill history con zeri
        for (int i = 0; i < HistoryLength; i++)
        {
            CpuHistory.Enqueue(0);
            RamHistory.Enqueue(0);
            GpuHistory.Enqueue(0);
            DiskHistory.Enqueue(0);
            NetHistory.Enqueue(0);
        }

        // Prime CPU baseline (la prima chiamata ritorna 0)
        _native.GetCpuPercent();
        // Prime disk/net baselines pure (le rate richiedono 2 sample)
        _native.GetDiskRates();
        _native.GetNetworkRates();

        // RamTotal è statico, lo leggiamo una volta
        var memInit = _native.GetMemory();
        RamTotalGb = memInit.TotalGb;

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => Tick();
    }

    public void Start()
    {
        _timer.Start();
        Tick();
    }

    public void Stop() => _timer.Stop();

    private void Tick()
    {
        var sw = Stopwatch.StartNew();
        try
        {
            Cpu = _native.GetCpuPercent();

            var mem = _native.GetMemory();
            RamUsedPercent = mem.UsedPercent;
            RamUsedGb = mem.UsedGb;
            RamAvailableGb = mem.AvailableGb;
            RamCachedGb = mem.CachedGb;
            RamCommittedGb = mem.CommittedGb;
            RamCommitLimitGb = mem.CommitLimitGb;
            PageFilePercent = mem.PageFilePercent;

            CpuFreqMhz = _native.GetCpuFreqMhz();

            var diskRates = _native.GetDiskRates();
            DiskReadBytesPerSec = diskRates.ReadBytesPerSec;
            DiskWriteBytesPerSec = diskRates.WriteBytesPerSec;
            double diskBps = diskRates.ReadBytesPerSec + diskRates.WriteBytesPerSec;

            var netRates = _native.GetNetworkRates();
            NetRecvBytesPerSec = netRates.ReceiveBytesPerSec;
            NetSentBytesPerSec = netRates.SendBytesPerSec;
            double netBps = netRates.ReceiveBytesPerSec + netRates.SendBytesPerSec;

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
            PerfLog.LogSlow("MetricsTick", sw.ElapsedMilliseconds, 50);
        }
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
    }
}
