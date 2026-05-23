using System;
using System.IO;

namespace ResourceMonitor.Services;

public static class PerfLog
{
    private static readonly string LogPath = Path.Combine(
        Path.GetTempPath(), "pulse-perf.log");

    private static readonly object _lock = new();

    public static void LogSlow(string op, long ms, int thresholdMs = 200)
    {
        if (ms < thresholdMs) return;
        try
        {
            lock (_lock)
            {
                File.AppendAllText(LogPath,
                    $"{DateTime.Now:HH:mm:ss.fff} [{op,-20}] {ms,5} ms{Environment.NewLine}");
            }
        }
        catch { }
    }

    public static string LogFilePath => LogPath;

    public static void Clear()
    {
        try { File.Delete(LogPath); } catch { }
    }
}
