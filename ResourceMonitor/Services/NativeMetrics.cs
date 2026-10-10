using System;
using System.Runtime.InteropServices;

namespace ResourceMonitor.Services;

/// <summary>
/// Win32 native wrappers per metriche di sistema.
/// Sostituisce PerformanceCounter (lento, basato su PDH/WMI) con syscall dirette.
/// Costo per chiamata: ~1ms vs ~3ms del PerformanceCounter.
/// </summary>
public sealed class NativeMetrics
{
    // ===== CPU =====

    private long _prevIdle, _prevKernel, _prevUser;
    private bool _cpuPrimed;

    /// <summary>CPU usage % aggregato. Prima chiamata serve come baseline (ritorna 0).</summary>
    public double GetCpuPercent()
    {
        if (!GetSystemTimes(out var idleFt, out var kernelFt, out var userFt)) return 0;
        long idle = ((long)idleFt.dwHighDateTime << 32) | (uint)idleFt.dwLowDateTime;
        long kernel = ((long)kernelFt.dwHighDateTime << 32) | (uint)kernelFt.dwLowDateTime;
        long user = ((long)userFt.dwHighDateTime << 32) | (uint)userFt.dwLowDateTime;

        if (!_cpuPrimed)
        {
            _prevIdle = idle; _prevKernel = kernel; _prevUser = user;
            _cpuPrimed = true;
            return 0;
        }

        long idleDelta = idle - _prevIdle;
        long totalDelta = (kernel - _prevKernel) + (user - _prevUser); // kernel include idle
        _prevIdle = idle; _prevKernel = kernel; _prevUser = user;

        if (totalDelta <= 0) return 0;
        // kernel time include idle, total = kernel + user, busy = total - idle
        return Math.Clamp((1.0 - (double)idleDelta / totalDelta) * 100.0, 0, 100);
    }

    // ===== CPU PER CORE =====

    // NtQuerySystemInformation con SystemProcessorPerformanceInformation (class 8): per ogni core logico del gruppo di processori
    // (al massimo 64) uno struct da 48 byte con IdleTime (8), KernelTime (8, idle compreso), UserTime (8), ... Una chiamata per tutti.
    private const int CorePerfSize = 48;
    private readonly byte[] _coreBuf = new byte[64 * CorePerfSize];
    private long[] _prevCoreIdle = Array.Empty<long>();
    private long[] _prevCoreTotal = Array.Empty<long>();

    /// <summary>CPU usage % di ogni core logico. Prima chiamata serve come baseline (ritorna zeri).</summary>
    public double[] GetCoreLoads()
    {
        int returned;
        var handle = GCHandle.Alloc(_coreBuf, GCHandleType.Pinned);
        try
        {
            if (NtQuerySystemInformation(8, handle.AddrOfPinnedObject(), _coreBuf.Length, out returned) != 0) return Array.Empty<double>();
        }
        finally
        {
            handle.Free();
        }

        int count = returned / CorePerfSize;
        var loads = new double[count];
        bool primed = _prevCoreIdle.Length == count;
        if (!primed) { _prevCoreIdle = new long[count]; _prevCoreTotal = new long[count]; }
        for (int i = 0; i < count; i++)
        {
            int at = i * CorePerfSize;
            long idle = BitConverter.ToInt64(_coreBuf, at);
            long total = BitConverter.ToInt64(_coreBuf, at + 8) + BitConverter.ToInt64(_coreBuf, at + 16);
            long totalDelta = total - _prevCoreTotal[i];
            if (primed && totalDelta > 0)
                loads[i] = Math.Clamp((1.0 - (double)(idle - _prevCoreIdle[i]) / totalDelta) * 100.0, 0, 100);
            _prevCoreIdle[i] = idle; _prevCoreTotal[i] = total;
        }
        return loads;
    }

    /// <summary>Per ognuno dei primi <paramref name="count"/> core logici, il core fisico a cui appartiene: i thread SMT dello stesso
    /// core hanno lo stesso numero. Dalla topologia di GetLogicalProcessorInformationEx, che non cambia: si legge una volta.
    /// Se la lettura fallisce ogni core logico fa core a sé.</summary>
    public static int[] GetPhysicalCores(int count)
    {
        var cores = new int[count];
        for (int i = 0; i < count; i++) cores[i] = -1 - i;
        int length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, IntPtr.Zero, ref length);
        if (length <= 0) return cores;
        IntPtr buf = Marshal.AllocHGlobal(length);
        try
        {
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, buf, ref length)) return cores;
            // SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: Relationship (4), Size (4), poi PROCESSOR_RELATIONSHIP con la GROUP_AFFINITY
            // del core a +32: Mask (8 byte, un bit per core logico) e Group (a +40). Conta il gruppo 0: fino a 64 core logici è l'unico
            for (int offset = 0, core = 0; offset < length; core++)
            {
                IntPtr entry = IntPtr.Add(buf, offset);
                int size = Marshal.ReadInt32(entry, 4);
                if (size <= 0) break;
                ulong mask = (ulong)Marshal.ReadInt64(entry, 32);
                if (Marshal.ReadInt16(entry, 40) == 0)
                {
                    for (int bit = 0; bit < count && bit < 64; bit++)
                        if ((mask & (1UL << bit)) != 0) cores[bit] = core;
                }
                offset += size;
            }
        }
        finally { Marshal.FreeHGlobal(buf); }
        return cores;
    }

    // ===== MEMORY =====

    public struct MemoryInfo
    {
        public double TotalGb;
        public double AvailableGb;
        public double UsedGb;
        public double UsedPercent;
        // la composizione della barra "Composizione memoria" di Gestione attività; NaN se Windows non dà le liste
        public double InUseGb;
        public double ModifiedGb;
        public double StandbyGb;
        public double FreeGb;
        public double CommittedGb;
        public double CommitLimitGb;
        public double PageFileUsedGb;
        public double PageFileTotalGb;
    }

    private readonly byte[] _memoryListBuf = new byte[176];
    private readonly byte[] _pageFileBuf = new byte[4096];

    public MemoryInfo GetMemory()
    {
        var info = new MemoryInfo { InUseGb = double.NaN, ModifiedGb = double.NaN, StandbyGb = double.NaN, FreeGb = double.NaN };

        var ms = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
        if (GlobalMemoryStatusEx(ref ms))
        {
            info.TotalGb = ms.ullTotalPhys / 1024d / 1024d / 1024d;
            info.AvailableGb = ms.ullAvailPhys / 1024d / 1024d / 1024d;
            info.UsedGb = info.TotalGb - info.AvailableGb;
            info.UsedPercent = ms.dwMemoryLoad;
        }

        double page = Environment.SystemPageSize / 1024d / 1024d / 1024d;

        // liste della memoria (SystemMemoryListInformation, classe 80), in pagine: ZeroPageCount (0), FreePageCount (8),
        // ModifiedPageCount (16), ModifiedNoWritePageCount (24), BadPageCount (32), PageCountByPriority[8] (40, lo standby).
        // "In uso" è quello che resta, come in Gestione attività
        if (info.TotalGb > 0 && Query(80, _memoryListBuf))
        {
            double standby = 0;
            for (int i = 0; i < 8; i++) standby += BitConverter.ToUInt64(_memoryListBuf, 40 + 8 * i);
            info.FreeGb = (BitConverter.ToUInt64(_memoryListBuf, 0) + BitConverter.ToUInt64(_memoryListBuf, 8)) * page;
            info.ModifiedGb = (BitConverter.ToUInt64(_memoryListBuf, 16) + BitConverter.ToUInt64(_memoryListBuf, 24)) * page;
            info.StandbyGb = standby * page;
            info.InUseGb = Math.Max(0, info.TotalGb - info.FreeGb - info.ModifiedGb - info.StandbyGb);
        }

        if (GetPerformanceInfo(out var pi, Marshal.SizeOf<PERFORMANCE_INFORMATION>()))
        {
            double pageSize = pi.PageSize.ToInt64();
            info.CommittedGb = pi.CommitTotal.ToInt64() * pageSize / 1024d / 1024d / 1024d;
            info.CommitLimitGb = pi.CommitLimit.ToInt64() * pageSize / 1024d / 1024d / 1024d;
        }

        // file di paging (SystemPageFileInformation, classe 18): uno struct per file con NextEntryOffset (0), TotalSize (4) e
        // TotalInUse (8) in pagine. Prima l'uso si stimava come impegnata meno RAM usata, che superava il file e dava 100 %
        if (Query(18, _pageFileBuf))
        {
            double total = 0, inUse = 0;
            for (int offset = 0; offset + 12 <= _pageFileBuf.Length;)
            {
                total += BitConverter.ToUInt32(_pageFileBuf, offset + 4);
                inUse += BitConverter.ToUInt32(_pageFileBuf, offset + 8);
                int next = BitConverter.ToInt32(_pageFileBuf, offset);
                if (next <= 0) break;
                offset += next;
            }
            info.PageFileTotalGb = total * page;
            info.PageFileUsedGb = inUse * page;
        }

        return info;
    }

    private static bool Query(int sysClass, byte[] buf)
    {
        Array.Clear(buf);
        var handle = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try { return NtQuerySystemInformation(sysClass, handle.AddrOfPinnedObject(), buf.Length, out _) == 0; }
        finally { handle.Free(); }
    }

    // ===== DISK =====

    private long _prevDiskRead, _prevDiskWrite;
    private DateTime _prevDiskSample = DateTime.MinValue;

    public struct DiskRates
    {
        public double ReadBytesPerSec;
        public double WriteBytesPerSec;
    }

    public DiskRates GetDiskRates()
    {
        var result = new DiskRates();
        // NtQuerySystemInformation con SystemPerformanceInformation (class 2)
        // Lo struct è grande (>200 byte). Riserviamo 1 KB di sicurezza.
        var buf = new byte[1024];
        var handle = GCHandle.Alloc(buf, GCHandleType.Pinned);
        try
        {
            int status = NtQuerySystemInformation(2, handle.AddrOfPinnedObject(), buf.Length, out _);
            if (status != 0) return result;
            // Offsets: IdleTime (8) + IoReadTransferCount (8) + IoWriteTransferCount (8) + IoOtherTransferCount (8) ...
            long readBytes = BitConverter.ToInt64(buf, 8);
            long writeBytes = BitConverter.ToInt64(buf, 16);

            var now = DateTime.UtcNow;
            if (_prevDiskSample != DateTime.MinValue)
            {
                double seconds = (now - _prevDiskSample).TotalSeconds;
                if (seconds > 0)
                {
                    result.ReadBytesPerSec = Math.Max(0, (readBytes - _prevDiskRead) / seconds);
                    result.WriteBytesPerSec = Math.Max(0, (writeBytes - _prevDiskWrite) / seconds);
                }
            }
            _prevDiskRead = readBytes;
            _prevDiskWrite = writeBytes;
            _prevDiskSample = now;
        }
        finally
        {
            handle.Free();
        }
        return result;
    }

    // ===== NETWORK =====

    private long _prevNetRecv, _prevNetSent;
    private DateTime _prevNetSample = DateTime.MinValue;

    public struct NetRates
    {
        public double ReceiveBytesPerSec;
        public double SendBytesPerSec;
    }

    public NetRates GetNetworkRates()
    {
        var result = new NetRates();
        if (GetIfTable2(out IntPtr tablePtr) != 0 || tablePtr == IntPtr.Zero) return result;
        try
        {
            int numEntries = Marshal.ReadInt32(tablePtr); // ULONG NumEntries
            // Padding allineamento 8 byte poi array di MIB_IF_ROW2
            int rowSize = Marshal.SizeOf<MIB_IF_ROW2>();
            long recv = 0, sent = 0;
            for (int i = 0; i < numEntries; i++)
            {
                IntPtr rowPtr = IntPtr.Add(tablePtr, 8 + i * rowSize);
                var row = Marshal.PtrToStructure<MIB_IF_ROW2>(rowPtr);
                // Filtra interfacce inattive / loopback / virtual
                if (row.OperStatus != 1) continue; // IfOperStatusUp = 1
                if (row.Type == 24) continue;       // IF_TYPE_SOFTWARE_LOOPBACK
                if (row.Type == 131) continue;      // IF_TYPE_TUNNEL
                recv += (long)row.InOctets;
                sent += (long)row.OutOctets;
            }

            var now = DateTime.UtcNow;
            if (_prevNetSample != DateTime.MinValue)
            {
                double seconds = (now - _prevNetSample).TotalSeconds;
                if (seconds > 0)
                {
                    result.ReceiveBytesPerSec = Math.Max(0, (recv - _prevNetRecv) / seconds);
                    result.SendBytesPerSec = Math.Max(0, (sent - _prevNetSent) / seconds);
                }
            }
            _prevNetRecv = recv;
            _prevNetSent = sent;
            _prevNetSample = now;
        }
        finally
        {
            FreeMibTable(tablePtr);
        }
        return result;
    }

    // ===== CPU FREQUENCY =====

    public double GetCpuFreqMhz()
    {
        // CallNtPowerInformation ProcessorInformation (class 11)
        int cpuCount = Environment.ProcessorCount;
        int structSize = Marshal.SizeOf<PROCESSOR_POWER_INFORMATION>();
        IntPtr buf = Marshal.AllocHGlobal(structSize * cpuCount);
        try
        {
            int status = CallNtPowerInformation(11, IntPtr.Zero, 0, buf, structSize * cpuCount);
            if (status != 0) return 0;
            long sum = 0;
            for (int i = 0; i < cpuCount; i++)
            {
                var info = Marshal.PtrToStructure<PROCESSOR_POWER_INFORMATION>(IntPtr.Add(buf, i * structSize));
                sum += info.CurrentMhz;
            }
            return (double)sum / cpuCount;
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    // ===== P/Invoke =====

    [StructLayout(LayoutKind.Sequential)]
    private struct FILETIME { public uint dwLowDateTime; public int dwHighDateTime; }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetSystemTimes(out FILETIME idleTime, out FILETIME kernelTime, out FILETIME userTime);

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

    [StructLayout(LayoutKind.Sequential)]
    private struct PERFORMANCE_INFORMATION
    {
        public int cb;
        public IntPtr CommitTotal;
        public IntPtr CommitLimit;
        public IntPtr CommitPeak;
        public IntPtr PhysicalTotal;
        public IntPtr PhysicalAvailable;
        public IntPtr SystemCache;
        public IntPtr KernelTotal;
        public IntPtr KernelPaged;
        public IntPtr KernelNonpaged;
        public IntPtr PageSize;
        public uint HandleCount;
        public uint ProcessCount;
        public uint ThreadCount;
    }

    [DllImport("psapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetPerformanceInfo(out PERFORMANCE_INFORMATION pPerformanceInfo, int cb);

    [DllImport("ntdll.dll")]
    private static extern int NtQuerySystemInformation(int sysClass, IntPtr buf, int bufSize, out int returned);

    private const int RelationProcessorCore = 0;

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetLogicalProcessorInformationEx(int relationshipType, IntPtr buffer, ref int returnedLength);

    [StructLayout(LayoutKind.Sequential)]
    private struct MIB_IF_ROW2
    {
        public ulong InterfaceLuid;
        public uint InterfaceIndex;
        public Guid InterfaceGuid;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 257)]
        public string Alias;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 257)]
        public string Description;
        public uint PhysicalAddressLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] PhysicalAddress;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] PermanentPhysicalAddress;
        public uint Mtu;
        public uint Type;
        public uint TunnelType;
        public uint MediaType;
        public uint PhysicalMediumType;
        public uint AccessType;
        public uint DirectionType;
        public byte InterfaceAndOperStatusFlags;
        public uint OperStatus;
        public uint AdminStatus;
        public uint MediaConnectState;
        public Guid NetworkGuid;
        public uint ConnectionType;
        public ulong TransmitLinkSpeed;
        public ulong ReceiveLinkSpeed;
        public ulong InOctets;
        public ulong InUcastPkts;
        public ulong InNUcastPkts;
        public ulong InDiscards;
        public ulong InErrors;
        public ulong InUnknownProtos;
        public ulong InUcastOctets;
        public ulong InMulticastOctets;
        public ulong InBroadcastOctets;
        public ulong OutOctets;
        public ulong OutUcastPkts;
        public ulong OutNUcastPkts;
        public ulong OutDiscards;
        public ulong OutErrors;
        public ulong OutUcastOctets;
        public ulong OutMulticastOctets;
        public ulong OutBroadcastOctets;
        public ulong InSpeed;
        public ulong OutSpeed;
        public uint InQueueLength;
        public uint OutQLen;
        public uint TransmitQueueLength;
    }

    [DllImport("iphlpapi.dll")]
    private static extern int GetIfTable2(out IntPtr table);

    [DllImport("iphlpapi.dll")]
    private static extern void FreeMibTable(IntPtr memory);

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESSOR_POWER_INFORMATION
    {
        public uint Number;
        public uint MaxMhz;
        public uint CurrentMhz;
        public uint MhzLimit;
        public uint MaxIdleState;
        public uint CurrentIdleState;
    }

    [DllImport("powrprof.dll")]
    private static extern int CallNtPowerInformation(int infoLevel, IntPtr inputBuffer, int inputBufSize, IntPtr outputBuffer, int outputBufSize);
}
