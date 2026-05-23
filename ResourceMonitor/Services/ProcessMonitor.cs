using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;

namespace ResourceMonitor.Services;

public sealed class ProcessSample
{
    public int Pid { get; init; }
    public string Name { get; init; } = string.Empty;
    public double CpuPercent { get; set; }
    public long WorkingSetBytes { get; set; }
}

public sealed class ProcessMonitor
{
    private readonly Dictionary<int, (TimeSpan total, DateTime ts)> _previous = new();
    private readonly int _cpuCount = Environment.ProcessorCount;

    public IReadOnlyList<ProcessSample> TopByCpu { get; private set; } = Array.Empty<ProcessSample>();
    public IReadOnlyList<ProcessSample> TopByMemory { get; private set; } = Array.Empty<ProcessSample>();

    public void Update()
    {
        if (Environment.GetEnvironmentVariable("PULSE_NO_PROC") == "1") return;
        var samples = new List<ProcessSample>(256);
        var now = DateTime.UtcNow;
        var current = new Dictionary<int, (TimeSpan, DateTime)>(256);

        foreach (var p in Process.GetProcesses())
        {
            try
            {
                if (p.Id == 0) continue;
                var name = p.ProcessName;
                TimeSpan total;
                try { total = p.TotalProcessorTime; }
                catch { p.Dispose(); continue; }

                long ws;
                try { ws = p.WorkingSet64; }
                catch { p.Dispose(); continue; }

                double cpu = 0;
                if (_previous.TryGetValue(p.Id, out var prev))
                {
                    var elapsed = (now - prev.ts).TotalSeconds;
                    if (elapsed > 0)
                    {
                        cpu = ((total - prev.total).TotalSeconds / elapsed) / _cpuCount * 100d;
                        if (cpu < 0) cpu = 0;
                        if (cpu > 100 * _cpuCount) cpu = 100 * _cpuCount;
                    }
                }
                current[p.Id] = (total, now);

                samples.Add(new ProcessSample
                {
                    Pid = p.Id,
                    Name = name,
                    CpuPercent = cpu,
                    WorkingSetBytes = ws
                });
            }
            catch { }
            finally { p.Dispose(); }
        }

        _previous.Clear();
        foreach (var kv in current) _previous[kv.Key] = kv.Value;

        TopByCpu = samples.OrderByDescending(s => s.CpuPercent).Take(6).ToArray();
        TopByMemory = samples.OrderByDescending(s => s.WorkingSetBytes).Take(6).ToArray();
    }
}
