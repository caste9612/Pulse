using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace ResourceMonitor.Services;

/// <summary>
/// Quale processo gira su quale core. Windows non lo tiene da leggere a basso costo (lo scheduler sposta i thread tra i core di
/// continuo), ma il kernel può annotare, 100 volte al secondo su ogni core logico, il thread che sta girando: i campioni
/// SampledProfile di ETW, da una sessione "system logger" in tempo reale. Qui si contano per core e per processo seguito (fino a
/// tre processi di Top CPU sopra il mezzo core, uno per colore); la sessione è aperta solo finché ce n'è uno (MetricsService
/// .SetCoreSampling). Serve Pulse da amministratore: senza, StartTrace nega l'accesso e non parte niente.
/// Se Pulse viene terminato di forza la sessione resta aperta: la chiude il prossimo avvio, prima di aprirne una nuova.
/// </summary>
public sealed class CoreSampler : IDisposable
{
    /// <summary>Processi seguiti, uno per colore.</summary>
    public const int Slots = 3;

    // un campione ogni 10 ms per core (in unità da 100 ns): 100 al secondo, invece dei 1000 del valore di Windows
    private const uint SampleInterval = 100_000;

    // conteggi per core: i processi seguiti, poi "tutti gli altri"; i campioni del thread idle non si contano
    private const int Columns = Slots + 1;

    private readonly string _name;
    private readonly int _cores;
    private readonly EventRecordCallback _callback;   // tenuto vivo: lo chiama ETW da codice nativo
    private readonly Dictionary<int, int> _threadProcess = new();
    private readonly IntPtr _loggerName;
    private readonly IntPtr _logfile;
    private readonly uint _previousInterval;
    private ulong _trace = InvalidTrace;
    private Thread? _thread;
    private volatile int[] _tracked = { -1, -1, -1 };
    private int[] _counts;
    private int[] _previous;
    private int _disposed;
    private int _checks;
    private long _sampled;
    private EventHandler? _onExit;

    private CoreSampler(string name, int cores, uint previousInterval)
    {
        _name = name;
        _cores = cores;
        _previousInterval = previousInterval;
        _counts = new int[cores * Columns];
        _previous = new int[cores * Columns];
        _callback = OnEvent;
        _loggerName = Marshal.StringToHGlobalUni(name);
        _logfile = Marshal.AllocHGlobal(Marshal.SizeOf<EVENT_TRACE_LOGFILEW>());
        var logfile = new EVENT_TRACE_LOGFILEW
        {
            LoggerName = _loggerName,
            ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD,
            EventRecordCallback = Marshal.GetFunctionPointerForDelegate(_callback)
        };
        Marshal.StructureToPtr(logfile, _logfile, false);
    }

    /// <summary>Il campionamento può partire solo da un Pulse elevato (un amministratore senza elevazione non basta).</summary>
    public static bool CanRun()
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            return new System.Security.Principal.WindowsPrincipal(identity).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    // "Pulse core sampling", "Pulse.Prova core sampling" per le build di prova
    private static string SessionName => $"{Path.GetFileNameWithoutExtension(Environment.ProcessPath) ?? "Pulse"} core sampling";

    /// <summary>Chiude la sessione rimasta aperta da un Pulse terminato di forza mentre campionava: senza consumatore il kernel
    /// continuerebbe a campionare finché un processo non torna sopra la soglia. Una chiamata, all'avvio.</summary>
    public static void StopOrphan() => StopSession(SessionName);

    /// <summary>Avvia il campionamento dei <paramref name="cores"/> core logici. Null se non parte: <paramref name="error"/> è
    /// l'errore di Windows (5 = Pulse non è da amministratore).</summary>
    public static CoreSampler? TryStart(int cores, out int error)
    {
        error = 0;
        if (IntPtr.Size != 8 || Marshal.SizeOf<EVENT_TRACE_LOGFILEW>() != 448) { error = 50; return null; }   // ERROR_NOT_SUPPORTED
        var name = SessionName;
        EnableProfilePrivilege();

        error = StartSession(name, out ulong session);
        if (error == ERROR_ALREADY_EXISTS)
        {
            // rimasta aperta da un Pulse terminato di forza
            StopSession(name);
            error = StartSession(name, out session);
        }
        if (error != 0)
        {
            if (error != ERROR_ACCESS_DENIED) PerfLog.Note($"core sampling not started: StartTrace error {error}");
            return null;
        }

        // l'intervallo di campionamento è di tutto il sistema (handle 0; se Windows lo rifiuta, quello della sessione): si legge
        // quello di prima per rimetterlo alla chiusura
        var interval = new TRACE_PROFILE_INTERVAL();
        uint previous = TraceQueryInformation(0, TraceSampledProfileIntervalInfo, ref interval, Marshal.SizeOf<TRACE_PROFILE_INTERVAL>(), out _) == 0
            ? interval.Interval : 0;
        interval = new TRACE_PROFILE_INTERVAL { Source = 0, Interval = SampleInterval };
        error = TraceSetInformation(0, TraceSampledProfileIntervalInfo, ref interval, Marshal.SizeOf<TRACE_PROFILE_INTERVAL>());
        if (error != 0) error = TraceSetInformation(session, TraceSampledProfileIntervalInfo, ref interval, Marshal.SizeOf<TRACE_PROFILE_INTERVAL>());
        if (error != 0)
        {
            // col valore di Windows sarebbero dieci volte tanti campioni: meglio niente colori
            PerfLog.Note($"core sampling not started: TraceSetInformation error {error}");
            StopSession(name);
            return null;
        }

        var sampler = new CoreSampler(name, cores, previous);
        sampler._trace = OpenTraceW(sampler._logfile);
        if (sampler._trace == InvalidTrace)
        {
            error = Marshal.GetLastWin32Error();
            PerfLog.Note($"core sampling not started: OpenTrace error {error}");
            sampler.Dispose();
            return null;
        }
        sampler._thread = new Thread(() =>
        {
            int status = ProcessTrace(new[] { sampler._trace }, 1, IntPtr.Zero, IntPtr.Zero);
            if (status != 0 && status != ERROR_CANCELLED && Volatile.Read(ref sampler._disposed) == 0)
                PerfLog.Note($"core sampling stopped: ProcessTrace error {status}");
        })
        {
            IsBackground = true,
            Name = "Pulse core sampling"
        };
        sampler._thread.Start();
        sampler._onExit = (_, _) => sampler.Dispose();
        AppDomain.CurrentDomain.ProcessExit += sampler._onExit;
        return sampler;
    }

    /// <summary>I processi da seguire, uno per colore (-1 = colore libero).</summary>
    public void Track(int[] pids)
    {
        var tracked = new int[Slots];
        for (int i = 0; i < Slots; i++) tracked[i] = i < pids.Length ? pids[i] : -1;
        // un colore passato a un altro processo non eredita i campioni del secondo prima
        if (!tracked.AsSpan().SequenceEqual(_tracked)) _previous = new int[_cores * Columns];
        _tracked = tracked;
    }

    /// <summary>Per ogni core, la parte dei suoi campioni non idle di ciascun processo seguito (<see cref="Slots"/> valori per
    /// core, 0–1), sugli ultimi due secondi; azzera il secondo in corso.</summary>
    public double[] TakeShares()
    {
        var counts = Interlocked.Exchange(ref _counts, new int[_cores * Columns]);
        if (_checks < 5)
        {
            // dopo cinque secondi senza un campione la sessione c'è ma non consegna niente: lo si annota una volta
            foreach (int count in counts) _sampled += count;
            if (++_checks == 5 && _sampled == 0) PerfLog.Note("core sampling: no samples in the first 5 s");
        }
        var shares = new double[_cores * Slots];
        for (int c = 0; c < _cores; c++)
        {
            int total = 0;
            for (int k = 0; k < Columns; k++) total += counts[c * Columns + k] + _previous[c * Columns + k];
            if (total == 0) continue;
            for (int k = 0; k < Slots; k++)
                shares[c * Slots + k] = (double)(counts[c * Columns + k] + _previous[c * Columns + k]) / total;
        }
        _previous = counts;
        return shares;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        if (_onExit is not null) AppDomain.CurrentDomain.ProcessExit -= _onExit;
        StopSession(_name);
        if (_trace != InvalidTrace) CloseTrace(_trace);
        _thread?.Join(2000);
        // rimette l'intervallo di prima, se nel frattempo nessun altro l'ha cambiato
        var interval = new TRACE_PROFILE_INTERVAL();
        if (_previousInterval != 0 && _previousInterval != SampleInterval &&
            TraceQueryInformation(0, TraceSampledProfileIntervalInfo, ref interval, Marshal.SizeOf<TRACE_PROFILE_INTERVAL>(), out _) == 0 &&
            interval.Interval == SampleInterval)
        {
            interval = new TRACE_PROFILE_INTERVAL { Source = 0, Interval = _previousInterval };
            TraceSetInformation(0, TraceSampledProfileIntervalInfo, ref interval, Marshal.SizeOf<TRACE_PROFILE_INTERVAL>());
        }
        Marshal.FreeHGlobal(_logfile);
        Marshal.FreeHGlobal(_loggerName);
    }

    // ===== eventi, sul thread di ProcessTrace =====

    private static readonly (long, long) PerfInfoGuid = GuidHalves("ce1dbfb4-137e-4da6-87b0-3f59aa102cbc");
    private static readonly (long, long) ThreadGuid = GuidHalves("3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c");

    private static (long, long) GuidHalves(string guid)
    {
        var bytes = new Guid(guid).ToByteArray();
        return (BitConverter.ToInt64(bytes, 0), BitConverter.ToInt64(bytes, 8));
    }

    // EVENT_RECORD: EventHeader (Flags a +4, ProviderId a +24, Version a +42, Opcode a +45), BufferContext a +80 (numero del
    // processore), UserDataLength a +86, UserData a +96
    private void OnEvent(IntPtr record)
    {
        try
        {
            var provider = (Marshal.ReadInt64(record, 24), Marshal.ReadInt64(record, 32));
            int opcode = Marshal.ReadByte(record, 45);
            IntPtr data = Marshal.ReadIntPtr(record, 96);
            int length = (ushort)Marshal.ReadInt16(record, 86);
            if (provider == PerfInfoGuid && opcode == 46)
            {
                // SampledProfile: InstructionPointer (puntatore), ThreadId
                int flags = (ushort)Marshal.ReadInt16(record, 4);
                int pointer = (flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0 ? 4 : 8;
                if (length < pointer + 4) return;
                int tid = Marshal.ReadInt32(data, pointer);
                if (tid == 0) return;
                int cpu = (flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX) != 0 ? (ushort)Marshal.ReadInt16(record, 80) : Marshal.ReadByte(record, 80);
                if (cpu >= _cores) return;
                int pid = ProcessOf(tid);
                int slot = pid < 0 ? -1 : Array.IndexOf(_tracked, pid);   // -1 nei colori liberi non è un processo
                Volatile.Read(ref _counts)[cpu * Columns + (slot < 0 ? Slots : slot)]++;
            }
            else if (provider == ThreadGuid && Marshal.ReadByte(record, 42) >= 2 && length >= 8)
            {
                // Thread, dalla versione 2: ProcessId, TThreadId. Start e DCStart (l'elenco dei thread già vivi all'avvio) li
                // aggiungono, End li toglie
                if (opcode is 1 or 3) _threadProcess[Marshal.ReadInt32(data, 4)] = Marshal.ReadInt32(data, 0);
                else if (opcode == 2) _threadProcess.Remove(Marshal.ReadInt32(data, 4));
            }
        }
        catch
        {
            // un'eccezione non deve tornare nel codice nativo di ETW
        }
    }

    private int ProcessOf(int tid)
    {
        if (_threadProcess.TryGetValue(tid, out int pid)) return pid;
        // thread non visto né all'avvio né alla nascita: lo si chiede a Windows una volta
        pid = -1;
        IntPtr handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, false, tid);
        if (handle != IntPtr.Zero)
        {
            pid = GetProcessIdOfThread(handle);
            CloseHandle(handle);
        }
        if (_threadProcess.Count > 200_000) _threadProcess.Clear();
        _threadProcess[tid] = pid;
        return pid;
    }

    // ===== sessione =====

    // EVENT_TRACE_PROPERTIES (120 byte) seguito dal nome della sessione
    private const int PropertiesSize = 120;
    private const int PropertiesBuffer = PropertiesSize + 1024;

    private static int StartSession(string name, out ulong session)
    {
        IntPtr properties = NewProperties();
        try
        {
            Marshal.StructureToPtr(Guid.NewGuid(), properties + 24, false);           // Wnode.Guid
            Marshal.WriteInt32(properties, 40, 1);                                    // Wnode.ClientContext: QueryPerformanceCounter
            Marshal.WriteInt32(properties, 48, 16);                                   // BufferSize, KB
            Marshal.WriteInt32(properties, 64, EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE);
            Marshal.WriteInt32(properties, 68, 1);                                    // FlushTimer, s
            Marshal.WriteInt32(properties, 72, EVENT_TRACE_FLAG_PROFILE | EVENT_TRACE_FLAG_THREAD);
            return StartTraceW(out session, name, properties);
        }
        finally { Marshal.FreeHGlobal(properties); }
    }

    private static void StopSession(string name)
    {
        IntPtr properties = NewProperties();
        try { ControlTraceW(0, name, properties, EVENT_TRACE_CONTROL_STOP); }
        finally { Marshal.FreeHGlobal(properties); }
    }

    private static IntPtr NewProperties()
    {
        IntPtr properties = Marshal.AllocHGlobal(PropertiesBuffer);
        for (int i = 0; i < PropertiesBuffer; i += 4) Marshal.WriteInt32(properties, i, 0);
        Marshal.WriteInt32(properties, 0, PropertiesBuffer);                          // Wnode.BufferSize
        Marshal.WriteInt32(properties, 44, WNODE_FLAG_TRACED_GUID);                   // Wnode.Flags
        Marshal.WriteInt32(properties, 116, PropertiesSize);                          // LoggerNameOffset
        return properties;
    }

    // i campioni del profilo richiedono SeSystemProfilePrivilege attivo nel token (un amministratore elevato ce l'ha, spento)
    private static void EnableProfilePrivilege()
    {
        if (!OpenProcessToken(new IntPtr(-1), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out IntPtr token)) return;
        try
        {
            if (!LookupPrivilegeValueW(null, "SeSystemProfilePrivilege", out long luid)) return;
            var state = new TOKEN_PRIVILEGES { PrivilegeCount = 1, Luid = luid, Attributes = SE_PRIVILEGE_ENABLED };
            AdjustTokenPrivileges(token, false, ref state, Marshal.SizeOf<TOKEN_PRIVILEGES>(), IntPtr.Zero, IntPtr.Zero);
        }
        finally { CloseHandle(token); }
    }

    // ===== P/Invoke =====

    private const ulong InvalidTrace = ulong.MaxValue;
    private const int ERROR_ACCESS_DENIED = 5;
    private const int ERROR_ALREADY_EXISTS = 183;
    private const int ERROR_CANCELLED = 1223;
    private const int WNODE_FLAG_TRACED_GUID = 0x00020000;
    private const int EVENT_TRACE_REAL_TIME_MODE = 0x00000100;
    private const int EVENT_TRACE_SYSTEM_LOGGER_MODE = 0x02000000;
    private const int EVENT_TRACE_FLAG_THREAD = 0x00000002;
    private const int EVENT_TRACE_FLAG_PROFILE = 0x01000000;
    private const uint EVENT_TRACE_CONTROL_STOP = 1;
    private const uint PROCESS_TRACE_MODE_REAL_TIME = 0x00000100;
    private const uint PROCESS_TRACE_MODE_EVENT_RECORD = 0x10000000;
    private const int EVENT_HEADER_FLAG_32_BIT_HEADER = 0x0020;
    private const int EVENT_HEADER_FLAG_PROCESSOR_INDEX = 0x0200;
    private const int TraceSampledProfileIntervalInfo = 5;
    private const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
    private const uint TOKEN_ADJUST_PRIVILEGES = 0x0020, TOKEN_QUERY = 0x0008;
    private const int SE_PRIVILEGE_ENABLED = 2;

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void EventRecordCallback(IntPtr record);

    // la struttura di OpenTrace: si usano solo il nome della sessione, il modo e la callback; EVENT_TRACE (88 byte) e
    // TRACE_LOGFILE_HEADER (280) restano opachi. 448 byte a 64 bit, controllati in TryStart
    [StructLayout(LayoutKind.Sequential, Size = 88)]
    private struct EVENT_TRACE { private long _align; }

    [StructLayout(LayoutKind.Sequential, Size = 280)]
    private struct TRACE_LOGFILE_HEADER { private long _align; }

    [StructLayout(LayoutKind.Sequential)]
    private struct EVENT_TRACE_LOGFILEW
    {
        public IntPtr LogFileName;
        public IntPtr LoggerName;
        public long CurrentTime;
        public uint BuffersRead;
        public uint ProcessTraceMode;
        public EVENT_TRACE CurrentEvent;
        public TRACE_LOGFILE_HEADER LogfileHeader;
        public IntPtr BufferCallback;
        public uint BufferSize;
        public uint Filled;
        public uint EventsLost;
        public IntPtr EventRecordCallback;
        public uint IsKernelTrace;
        public IntPtr Context;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TRACE_PROFILE_INTERVAL { public uint Source; public uint Interval; }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct TOKEN_PRIVILEGES { public int PrivilegeCount; public long Luid; public int Attributes; }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int StartTraceW(out ulong session, string name, IntPtr properties);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int ControlTraceW(ulong session, string name, IntPtr properties, uint control);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern ulong OpenTraceW(IntPtr logfile);

    [DllImport("advapi32.dll")]
    private static extern int ProcessTrace(ulong[] handles, uint count, IntPtr start, IntPtr end);

    [DllImport("advapi32.dll")]
    private static extern int CloseTrace(ulong trace);

    [DllImport("advapi32.dll")]
    private static extern int TraceSetInformation(ulong session, int infoClass, ref TRACE_PROFILE_INTERVAL info, int length);

    [DllImport("advapi32.dll")]
    private static extern int TraceQueryInformation(ulong session, int infoClass, ref TRACE_PROFILE_INTERVAL info, int length, out int returned);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValueW(string? system, string name, out long luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES state, int length, IntPtr previous, IntPtr returned);

    [DllImport("kernel32.dll")]
    private static extern IntPtr OpenThread(uint access, bool inherit, int threadId);

    [DllImport("kernel32.dll")]
    private static extern int GetProcessIdOfThread(IntPtr thread);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
}
