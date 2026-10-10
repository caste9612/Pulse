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
    private readonly bool _samplingPossible;
    private volatile CoreSampler? _sampler;
    private volatile bool _samplingFailed;
    private int _samplingSwitching;
    private int[] _trackedPids = { -1, -1, -1 };

    public double Cpu { get; private set; }
    public double[] CpuCores { get; private set; } = Array.Empty<double>();
    /// <summary>Per ogni core logico di <see cref="CpuCores"/>, il core fisico a cui appartiene.</summary>
    public int[] CpuCorePhysical { get; }
    /// <summary>Quanti core pieni vale il carico: la somma dei carichi dei core logici.</summary>
    public double CpuCoresInUse { get; private set; }
    /// <summary>Per ogni core logico, la parte del carico di ciascun processo seguito (<see cref="CoreSampler.Slots"/> valori per
    /// core, 0–1); null senza il campionamento del kernel.</summary>
    public double[]? CpuCoreShares { get; private set; }
    /// <summary>Spento con PULSE_NO_CORE_SAMPLING=1.</summary>
    public bool CoreSamplingDisabled { get; }
    /// <summary>Il campionamento dei core si può accendere: Pulse da amministratore, non spento, nessun avvio fallito.</summary>
    public bool CoreSamplingAvailable => _samplingPossible && !_samplingFailed;
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
        // idem per i singoli core; quali core logici stanno sullo stesso core fisico non cambia, si legge una volta
        CpuCorePhysical = NativeMetrics.GetPhysicalCores(_native.GetCoreLoads().Length);
        // quale processo gira su quale core: solo da amministratore, e acceso solo quando c'è un processo da seguire
        // (SetCoreSampling, vedi CoreSampler)
        CoreSamplingDisabled = Environment.GetEnvironmentVariable("PULSE_NO_CORE_SAMPLING") == "1";
        _samplingPossible = !CoreSamplingDisabled && CoreSampler.CanRun();
        if (_samplingPossible) CoreSampler.StopOrphan();
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
            CpuCores = _native.GetCoreLoads();
            CpuCoresInUse = CpuCores.Sum() / 100;
            CpuCoreShares = _sampler?.TakeShares();

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

    /// <summary>I processi da seguire nel campionamento dei core, uno per colore (-1 = colore libero).</summary>
    public void TrackProcesses(int[] pids)
    {
        _trackedPids = (int[])pids.Clone();
        _sampler?.Track(_trackedPids);
    }

    /// <summary>Accende o spegne il campionamento dei core. Aprire e chiudere la sessione del kernel prende qualche millisecondo
    /// (la chiusura aspetta il thread di ETW): lo si fa fuori dal thread della UI, una cosa alla volta.</summary>
    public void SetCoreSampling(bool on)
    {
        if (!CoreSamplingAvailable || on == (_sampler is not null)) return;
        if (Interlocked.Exchange(ref _samplingSwitching, 1) == 1) return;
        Task.Run(() =>
        {
            try
            {
                if (on)
                {
                    var sampler = CoreSampler.TryStart(CpuCorePhysical.Length, out _);
                    if (sampler is null) { _samplingFailed = true; return; }   // il motivo è nel perf log
                    sampler.Track(_trackedPids);
                    _sampler = sampler;
                }
                else
                {
                    var sampler = _sampler;
                    _sampler = null;
                    sampler?.Dispose();
                }
            }
            finally { Volatile.Write(ref _samplingSwitching, 0); }
        });
    }

    public void Dispose()
    {
        _timer.Stop();
        _sampler?.Dispose();
    }
}
