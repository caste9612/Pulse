using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;

namespace ResourceMonitor.Services;

/// <summary>
/// Pulse partito senza diritti di amministratore quando all'installazione è stato scelto "Start with administrator privileges":
/// a fine installazione o aggiornamento (l'installer avvia Pulse da utente normale), dal menu Start, dall'icona del desktop.
/// L'installer ha creato il task "Pulse", che parte con i diritti massimi senza chiedere UAC: Pulse avvia quel task e si chiude.
/// Lo fa solo se tutto torna (amministratore con UAC in un processo senza diritti, task attivo, con i diritti massimi, dello
/// stesso utente e che avvia proprio questo exe) e al più una volta al minuto; se il Pulse elevato non compare entro dieci
/// secondi resta questo, senza diritti, come prima. PULSE_NO_ADMIN_START=1 lo spegne.
/// </summary>
internal static class AdminStart
{
    /// <summary>Il task di accesso che l'installer registra con startup_admin (MyAppTaskName in installer/pulse.iss).</summary>
    private const string TaskName = "Pulse";

    public enum Result
    {
        /// <summary>Niente da fare: Pulse continua in questo processo.</summary>
        None,
        /// <summary>Il Pulse da amministratore è partito: questo processo esce.</summary>
        HandedOff,
        /// <summary>Il task non ha portato un Pulse elevato: si continua qui, ma l'istanza unica è stata liberata e va ripresa.</summary>
        Failed,
    }

    private const int TokenElevationType = 18, TokenElevationTypeLimited = 3;
    private const int TaskRunLevelHighest = 1, TaskActionExec = 0;
    private const int TaskNotFound = unchecked((int)0x80070002);
    private static readonly TimeSpan Appear = TimeSpan.FromSeconds(10);

    /// <param name="args">Gli argomenti dell'avvio: le ripartenze (--restart, --after-crash) hanno già i loro diritti e un
    /// argomento come --appearance non passerebbe dal task, quindi si passa al task solo per un avvio semplice.</param>
    /// <param name="releaseInstance">Libera l'istanza unica subito prima di avviare il task: il Pulse elevato la deve prendere.</param>
    /// <param name="instanceName">Il nome dell'istanza unica, da cui si vede che il Pulse elevato è partito.</param>
    public static Result TryHandOff(string[] args, Action releaseInstance, string instanceName)
    {
        if (args.Length > 0 || Environment.GetEnvironmentVariable("PULSE_NO_ADMIN_START") == "1" || !IsLimitedAdmin()) return Result.None;
        object? service = null;
        bool released = false;
        try
        {
            var type = Type.GetTypeFromProgID("Schedule.Service");
            if (type is null) return Result.None;
            service = Activator.CreateInstance(type);
            dynamic scheduler = service!;
            scheduler.Connect();
            dynamic task;
            try { task = scheduler.GetFolder("\\").GetTask(TaskName); }
            catch (Exception ex) when (ex.HResult == TaskNotFound) { return Result.None; }   // nessun task: Pulse resta com'è

            string? unusable = Unusable(task);
            if (unusable is not null)
            {
                PerfLog.Note($"admin start: task \"{TaskName}\" not used ({unusable})");
                return Result.None;
            }
            if (!SettingsService.AllowAdminHandOff())
            {
                PerfLog.Note("admin start: skipped, already handed off in the last minute");
                return Result.None;
            }

            releaseInstance();
            released = true;
            task.Run(DBNull.Value);   // VT_NULL: nessun parametro per l'azione
            if (WaitForInstance(instanceName))
            {
                PerfLog.Note($"admin start: handed off to task \"{TaskName}\"");
                return Result.HandedOff;
            }
            PerfLog.Note($"admin start: no elevated Pulse within {Appear.TotalSeconds:0} s, staying without administrator rights");
            return Result.Failed;
        }
        catch (Exception ex)
        {
            PerfLog.Note($"admin start: {ex.GetType().Name}: {ex.Message.Trim()}");
            return released ? Result.Failed : Result.None;
        }
        finally
        {
            if (service is not null) Marshal.FinalReleaseComObject(service);
        }
    }

    // perché il task non va avviato, o null se va bene: deve far partire proprio questo Pulse, elevato, per questo utente
    private static string? Unusable(dynamic task)
    {
        if (!(bool)task.Enabled) return "disabled";
        dynamic definition = task.Definition;
        if ((int)definition.Principal.RunLevel != TaskRunLevelHighest) return "not run with highest privileges";
        if (!IsCurrentUser((string?)definition.Principal.UserId)) return "another user";
        dynamic actions = definition.Actions;
        if ((int)actions.Count < 1) return "no action";
        dynamic action = actions[1];
        if ((int)action.Type != TaskActionExec) return "not a program";
        string path = Environment.ExpandEnvironmentVariables(((string?)action.Path ?? "").Trim().Trim('"'));
        string? self = Environment.ProcessPath;
        if (self is null || path.Length == 0 || !string.Equals(Path.GetFullPath(path), Path.GetFullPath(self), StringComparison.OrdinalIgnoreCase))
            return $"starts {path}";
        return null;
    }

    // il Task Scheduler restituisce l'utente come l'ha registrato chi ha creato il task: solo il nome ("wcast", il task di
    // schtasks della 1.2.0), dominio e nome, il SID (il modello dell'installer). Le forme comuni si confrontano senza chiedere
    // al sistema; le altre (UPN) passano dal SID
    private static bool IsCurrentUser(string? userId)
    {
        if (string.IsNullOrWhiteSpace(userId)) return false;
        try
        {
            using var me = WindowsIdentity.GetCurrent();
            string name = me.Name;
            string shortName = name[(name.IndexOf('\\') + 1)..];
            if (string.Equals(userId, me.User?.Value, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(userId, name, StringComparison.OrdinalIgnoreCase) ||
                string.Equals(userId, shortName, StringComparison.OrdinalIgnoreCase))
                return true;
            var sid = userId.StartsWith("S-1-", StringComparison.OrdinalIgnoreCase)
                ? new SecurityIdentifier(userId)
                : (SecurityIdentifier)new NTAccount(userId).Translate(typeof(SecurityIdentifier));
            return sid == me.User;
        }
        catch { return false; }
    }

    // amministratore con UAC in un processo senza diritti: è il caso in cui il task con i diritti massimi parte elevato. Con UAC
    // spento o da utente normale il task partirebbe senza diritti come questo processo, e passarci non servirebbe
    private static bool IsLimitedAdmin()
    {
        try
        {
            using var me = WindowsIdentity.GetCurrent();
            return GetTokenInformation(me.Token, TokenElevationType, out int type, sizeof(int), out _) && type == TokenElevationTypeLimited;
        }
        catch { return false; }
    }

    // il Pulse elevato prende l'istanza unica appena parte: la si cerca per qualche secondo. Se Windows nega l'accesso al suo
    // mutex, il mutex c'è (è di un processo elevato)
    private static bool WaitForInstance(string name)
    {
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < Appear)
        {
            try
            {
                if (Mutex.TryOpenExisting(name, out var mutex)) { mutex.Dispose(); return true; }
            }
            catch (UnauthorizedAccessException) { return true; }
            catch { }
            Thread.Sleep(100);
        }
        return false;
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returnLength);
}
