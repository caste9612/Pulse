using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace ResourceMonitor.Services;

public sealed class DriveInfoSnapshot
{
    public string Name { get; init; } = string.Empty;
    public long FreeBytes { get; init; }
    public long TotalBytes { get; init; }
    public double FreeGb => FreeBytes / 1024d / 1024 / 1024;
    public double TotalGb => TotalBytes / 1024d / 1024 / 1024;
    public double UsedPercent => TotalBytes > 0 ? (1d - FreeBytes / (double)TotalBytes) * 100d : 0;
}

public sealed class DriveMonitor
{
    public IReadOnlyList<DriveInfoSnapshot> Drives { get; private set; } = Array.Empty<DriveInfoSnapshot>();

    public void Update()
    {
        try
        {
            Drives = DriveInfo.GetDrives()
                .Where(d => d.IsReady && d.DriveType is DriveType.Fixed or DriveType.Removable)
                .Select(d => new DriveInfoSnapshot
                {
                    Name = d.Name.TrimEnd('\\'),
                    FreeBytes = d.AvailableFreeSpace,
                    TotalBytes = d.TotalSize
                })
                .ToArray();
        }
        catch { }
    }
}
