using System;
using System.IO;

namespace ResourceMonitor.Services;

/// <summary>Il registro del processo dei sensori, %TEMP%\pulse2-sensors.log: lo stesso nome di classe che CoreSampler usa in
/// Pulse 1.x, ma un file suo, così non scrive in quello del Pulse installato.</summary>
public static class PerfLog
{
    private static readonly string LogPath = Path.Combine(Path.GetTempPath(), "pulse2-sensors.log");
    private static readonly object Lock = new();

    public static void Note(string text)
    {
        try { lock (Lock) File.AppendAllText(LogPath, $"{DateTime.Now:dd/MM HH:mm:ss} {text}{Environment.NewLine}"); }
        catch { }
    }
}
