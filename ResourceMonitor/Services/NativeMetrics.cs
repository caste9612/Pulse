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

    // ===== MEMORY =====

    public struct MemoryInfo
    {
        public double TotalGb;
        public double AvailableGb;
        public double UsedGb;
        public double UsedPercent;
        public double CachedGb;
        public double CommittedGb;
        public double CommitLimitGb;
        public double PageFilePercent;
    }

    public MemoryInfo GetMemory()
    {
        var info = new MemoryInfo();

        var ms = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
        if (GlobalMemoryStatusEx(ref ms))
        {
            info.TotalGb = ms.ullTotalPhys / 1024d / 1024d / 1024d;
            info.AvailableGb = ms.ullAvailPhys / 1024d / 1024d / 1024d;
            info.UsedGb = info.TotalGb - info.AvailableGb;
            info.UsedPercent = ms.dwMemoryLoad;

            double pageFileTotal = ms.ullTotalPageFile;
            double pageFileAvail = ms.ullAvailPageFile;
            if (pageFileTotal > 0)
            {
                // Pagefile usage = (totale page+phys - disponibile page+phys) - (totale phys - disp phys), divided by file portion
                // Approssimazione: % use of page file = used_pf / total_pf
                // total_pf reale = ullTotalPageFile - ullTotalPhys
                double pagefileBytes = pageFileTotal - ms.ullTotalPhys;
                double pagefileUsed = (pageFileTotal - pageFileAvail) - (ms.ullTotalPhys - ms.ullAvailPhys);
                if (pagefileBytes > 0 && pagefileUsed > 0)
                    info.PageFilePercent = Math.Clamp(pagefileUsed / pagefileBytes * 100, 0, 100);
            }
        }

        if (GetPerformanceInfo(out var pi, Marshal.SizeOf<PERFORMANCE_INFORMATION>()))
        {
            double pageSize = pi.PageSize.ToInt64();
            info.CommittedGb = pi.CommitTotal.ToInt64() * pageSize / 1024d / 1024d / 1024d;
            info.CommitLimitGb = pi.CommitLimit.ToInt64() * pageSize / 1024d / 1024d / 1024d;
            info.CachedGb = pi.SystemCache.ToInt64() * pageSize / 1024d / 1024d / 1024d;
        }

        return info;
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
