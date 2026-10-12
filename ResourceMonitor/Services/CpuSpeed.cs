using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace ResourceMonitor.Services;

/// <summary>
/// La velocità media della CPU come la mostra Gestione attività: la frequenza nominale per "% Processor Performance" del
/// counterset "Processor Information" del kernel, letto con l'API consumer di PerfLib V2 (advapi32), senza PDH. Serve quando
/// LibreHardwareMonitor non ha le frequenze dei core (senza il driver PawnIO o senza diritti di amministratore): l'altra fonte,
/// CallNtPowerInformation, sui processori Intel recenti dà sempre la frequenza base (3,50 GHz fissi sull'i9-11900K, che a
/// riposo ne fa circa 2,2 e sotto carico più di 4). Misurato il 12/10/2026 sull'i9-11900K: gli stessi valori di PDH, che però
/// costava 6,4 MB di memoria privata e 420 ms al primo avvio; qui meno di 1 MB, circa 0,4 ms a lettura.
/// </summary>
public sealed class CpuSpeed : IDisposable
{
    // il counterset "Processor Information" del kernel; i contatori si cercano per nome inglese, non per numero
    private static readonly Guid ProcessorInformation = new("b4fc721a-0378-476f-89ba-a5a79f810b36");
    private const string PerformanceName = "% Processor Performance", FrequencyName = "Processor Frequency";
    private const uint PERF_REG_COUNTERSET_STRUCT = 1, PERF_REG_COUNTER_ENGLISH_NAMES = 10;
    private const uint PERF_WILDCARD_INSTANCE = 0xFFFFFFFF;

    private readonly object _lock = new();
    private readonly byte[] _data = new byte[512];
    private IntPtr _query;
    private ulong _prevPerformance, _prevBase;
    private bool _primed;

    private CpuSpeed(IntPtr query) => _query = query;

    /// <summary>Apre la lettura e prende il primo campione, così la prima <see cref="ReadMhz"/> ha già un valore. Null se il
    /// counterset o i contatori non ci sono: Pulse resta sulla frequenza di CallNtPowerInformation.</summary>
    public static CpuSpeed? TryOpen()
    {
        IntPtr query = IntPtr.Zero;
        try
        {
            var names = CounterNames();
            var info = Registration(PERF_REG_COUNTERSET_STRUCT);
            // PERF_COUNTERSET_REG_INFO (32 byte, NumCounters a +24), poi un PERF_COUNTER_REG_INFO da 48 byte per contatore:
            // CounterId a +0, BaseCounterId a +24
            if (info.Length < 32) return null;
            int count = BitConverter.ToInt32(info, 24);
            uint performance = uint.MaxValue, performanceBase = uint.MaxValue, frequency = uint.MaxValue;
            for (int i = 0; i < count && 32 + (i + 1) * 48 <= info.Length; i++)
            {
                uint id = BitConverter.ToUInt32(info, 32 + i * 48);
                if (!names.TryGetValue(id, out var name)) continue;
                if (name == PerformanceName) { performance = id; performanceBase = BitConverter.ToUInt32(info, 32 + i * 48 + 24); }
                else if (name == FrequencyName) frequency = id;
            }
            if (performance == uint.MaxValue || performanceBase == uint.MaxValue || frequency == uint.MaxValue) return null;

            if (PerfOpenQueryHandle(null, out query) != 0) return null;
            // nell'ordine in cui tornano i dati: il contatore, la sua base, la frequenza nominale
            foreach (var id in new[] { performance, performanceBase, frequency })
            {
                if (!Add(query, id, "_Total")) { PerfCloseQueryHandle(query); return null; }
            }
            var speed = new CpuSpeed(query);
            speed.ReadMhz();
            return speed;
        }
        catch (Exception ex)
        {
            PerfLog.Note($"cpu speed not available: {ex.Message}");
            if (query != IntPtr.Zero) PerfCloseQueryHandle(query);
            return null;
        }
    }

    /// <summary>La velocità media dall'ultima lettura, in MHz: la frequenza nominale per la prestazione media dei core mentre
    /// lavorano, che col turbo supera il 100 %. Null alla prima lettura o se i contatori non rispondono.</summary>
    public double? ReadMhz()
    {
        lock (_lock)
        {
            if (_query == IntPtr.Zero || PerfQueryCounterData(_query, _data, (uint)_data.Length, out _) != 0) return null;
            // PERF_DATA_HEADER (48 byte, dwNumCounters a +4), poi per contatore PERF_COUNTER_HEADER (dwStatus, dwType, dwSize,
            // Reserved) e, per un'istanza sola, PERF_COUNTER_DATA (dwDataSize, dwSize) seguito dal valore
            if (BitConverter.ToInt32(_data, 4) != 3) return null;
            Span<ulong> values = stackalloc ulong[3];
            Span<bool> wide = stackalloc bool[3];
            for (int i = 0, at = 48; i < 3; i++)
            {
                // dwStatus 0 e dwType PERF_SINGLE_COUNTER (1): un valore solo, quello dell'istanza _Total
                if (at + 24 > _data.Length || BitConverter.ToInt32(_data, at) != 0 || BitConverter.ToInt32(_data, at + 4) != 1) return null;
                int dataSize = BitConverter.ToInt32(_data, at + 16);
                wide[i] = dataSize >= 8;
                values[i] = wide[i] ? BitConverter.ToUInt64(_data, at + 24) : BitConverter.ToUInt32(_data, at + 24);
                at += BitConverter.ToInt32(_data, at + 8);
            }
            ulong performance = Delta(values[0], _prevPerformance, wide[0]), baseDelta = Delta(values[1], _prevBase, wide[1]);
            bool primed = _primed;
            _prevPerformance = values[0];
            _prevBase = values[1];
            _primed = true;
            if (!primed || baseDelta == 0 || values[2] == 0) return null;
            return values[2] * ((double)performance / baseDelta) / 100;
        }
    }

    // un contatore a 32 bit che ha fatto il giro riparte da zero: la differenza si prende modulo 2^32
    private static ulong Delta(ulong current, ulong previous, bool wide) =>
        wide ? current - previous : (uint)(current - previous);

    public void Dispose()
    {
        lock (_lock)
        {
            if (_query == IntPtr.Zero) return;
            PerfCloseQueryHandle(_query);
            _query = IntPtr.Zero;
        }
    }

    // PERF_COUNTER_IDENTIFIER (40 byte: CounterSetGuid, Status a +16, Size a +20, CounterId a +24, InstanceId a +28, Index,
    // Reserved) seguito dal nome dell'istanza, allineato a 8 byte
    private static bool Add(IntPtr query, uint counter, string instance)
    {
        var name = Encoding.Unicode.GetBytes(instance + "\0");
        int size = (40 + name.Length + 7) & ~7;
        var id = new byte[size];
        ProcessorInformation.ToByteArray().CopyTo(id, 0);
        BitConverter.GetBytes(size).CopyTo(id, 20);
        BitConverter.GetBytes(counter).CopyTo(id, 24);
        BitConverter.GetBytes(PERF_WILDCARD_INSTANCE).CopyTo(id, 28);
        name.CopyTo(id, 40);
        return PerfAddCounters(query, id, (uint)size) == 0 && BitConverter.ToUInt32(id, 16) == 0;
    }

    // PERF_STRING_BUFFER_HEADER (dwSize, dwCounters), poi un PERF_STRING_COUNTER_HEADER (dwCounterId, dwOffset dall'inizio) per
    // contatore e le stringhe UTF-16
    private static Dictionary<uint, string> CounterNames()
    {
        var names = new Dictionary<uint, string>();
        var buffer = Registration(PERF_REG_COUNTER_ENGLISH_NAMES);
        if (buffer.Length < 8) return names;
        int counters = BitConverter.ToInt32(buffer, 4);
        for (int i = 0; i < counters && 16 + 8 * i <= buffer.Length; i++)
        {
            uint id = BitConverter.ToUInt32(buffer, 8 + 8 * i);
            int offset = BitConverter.ToInt32(buffer, 12 + 8 * i);
            if (offset <= 0 || offset >= buffer.Length) continue;
            int end = offset;
            while (end + 1 < buffer.Length && (buffer[end] != 0 || buffer[end + 1] != 0)) end += 2;
            names[id] = Encoding.Unicode.GetString(buffer, offset, end - offset);
        }
        return names;
    }

    private static byte[] Registration(uint type)
    {
        var set = ProcessorInformation;
        PerfQueryCounterSetRegistrationInfo(null, ref set, type, 0, null, 0, out uint size);
        if (size == 0) return Array.Empty<byte>();
        var buffer = new byte[size];
        return PerfQueryCounterSetRegistrationInfo(null, ref set, type, 0, buffer, size, out _) == 0 ? buffer : Array.Empty<byte>();
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern uint PerfQueryCounterSetRegistrationInfo(string? machine, ref Guid counterSet, uint requestCode, uint languageId,
        byte[]? buffer, uint size, out uint actual);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern uint PerfOpenQueryHandle(string? machine, out IntPtr query);

    [DllImport("advapi32.dll")]
    private static extern uint PerfAddCounters(IntPtr query, byte[] counters, uint size);

    [DllImport("advapi32.dll")]
    private static extern uint PerfQueryCounterData(IntPtr query, byte[] buffer, uint size, out uint used);

    [DllImport("advapi32.dll")]
    private static extern uint PerfCloseQueryHandle(IntPtr query);
}
