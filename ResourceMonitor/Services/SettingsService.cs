using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace ResourceMonitor.Services;

public sealed class AppSettings
{
    public double Left { get; set; } = double.NaN;
    public double Top { get; set; } = double.NaN;
    public double Width { get; set; } = 280;
    public double Height { get; set; } = 220;
    public bool IsVisible { get; set; } = true;
    public bool Topmost { get; set; } = true;
    public bool StartHidden { get; set; } = false;
    public bool AutoStart { get; set; } = false;
}

public static class SettingsService
{
    private static readonly string NewDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Pulse");
    private static readonly string LegacyDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ResourceMonitor");

    private static readonly string SettingsPath = Path.Combine(NewDir, "settings.json");
    private static readonly string LegacyPath = Path.Combine(LegacyDir, "settings.json");

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };

    public static AppSettings Load()
    {
        try
        {
            // Migrazione: copia old → new se nuovo non esiste
            if (!File.Exists(SettingsPath) && File.Exists(LegacyPath))
            {
                Directory.CreateDirectory(NewDir);
                File.Copy(LegacyPath, SettingsPath);
            }

            if (!File.Exists(SettingsPath)) return new AppSettings();
            var json = File.ReadAllText(SettingsPath);
            return JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
        }
        catch
        {
            return new AppSettings();
        }
    }

    public static void Save(AppSettings settings)
    {
        try
        {
            Directory.CreateDirectory(NewDir);
            File.WriteAllText(SettingsPath, JsonSerializer.Serialize(settings, JsonOpts));
        }
        catch { }
    }

    /// <summary>Una ripartenza dopo un crash (dal task "Pulse riavvio") è ammessa se nei dieci minuti prima ce ne sono state meno
    /// di tre, e in quel caso la si annota: un Pulse che va in crash appena parte non riparte all'infinito.</summary>
    public static bool AllowCrashRestart()
    {
        var path = Path.Combine(NewDir, "riavvii-dopo-crash.txt");
        try
        {
            var now = DateTime.UtcNow;
            var recent = new List<DateTime>();
            if (File.Exists(path))
            {
                foreach (var line in File.ReadAllLines(path))
                {
                    if (DateTime.TryParse(line, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind, out var when) &&
                        now - when < TimeSpan.FromMinutes(10))
                        recent.Add(when);
                }
            }
            if (recent.Count >= 3) return false;
            recent.Add(now);
            Directory.CreateDirectory(NewDir);
            File.WriteAllLines(path, recent.Select(t => t.ToString("O", CultureInfo.InvariantCulture)));
            return true;
        }
        catch
        {
            return true;
        }
    }
}
