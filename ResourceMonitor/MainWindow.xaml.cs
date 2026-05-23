using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using ResourceMonitor.Services;

namespace ResourceMonitor;

public partial class MainWindow : Window
{
    private readonly MetricsService _metrics;
    private readonly HardwareMonitor _hw;
    private readonly ProcessMonitor _proc;
    private readonly NetworkMonitor _net;
    private readonly DriveMonitor _drive;
    private readonly AppSettings _settings;

    private readonly DispatcherTimer _slowTimer;
    private readonly DispatcherTimer _procTimer;
    private readonly DispatcherTimer _driveTimer;
    private bool _settingsLoaded;
    private bool _suppressSave;
    private bool _slowRunning;
    private bool _procRunning;
    private DateTime _appStart = DateTime.UtcNow;
    private DateTime _machineBoot;

    public MainWindow(MetricsService metrics, HardwareMonitor hw, ProcessMonitor proc,
                      NetworkMonitor net, DriveMonitor drive, AppSettings settings)
    {
        InitializeComponent();
        _metrics = metrics;
        _hw = hw;
        _proc = proc;
        _net = net;
        _drive = drive;
        _settings = settings;
        _metrics.Updated += OnMetricsUpdated;

        _machineBoot = DateTime.Now - TimeSpan.FromMilliseconds(Environment.TickCount64);

        _slowTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
        _slowTimer.Tick += async (_, _) => await TickSlowAsync();

        _procTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        _procTimer.Tick += async (_, _) => await TickProcessesAsync();

        _driveTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
        _driveTimer.Tick += (_, _) => BuildDriveList();

        Loaded += OnLoaded;
        Closing += OnClosing;
        SizeChanged += (_, _) => { if (_settingsLoaded) SaveGeometry(); };
        LocationChanged += (_, _) => { if (_settingsLoaded) SaveGeometry(); };
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        _suppressSave = true;
        if (!double.IsNaN(_settings.Width) && _settings.Width > 0) Width = _settings.Width;
        if (!double.IsNaN(_settings.Height) && _settings.Height > 0) Height = _settings.Height;
        if (!double.IsNaN(_settings.Left) && !double.IsNaN(_settings.Top))
        {
            Left = _settings.Left;
            Top = _settings.Top;
            ClampToVisibleScreen();
        }
        else
        {
            var area = SystemParameters.WorkArea;
            Left = area.Right - Width - 12;
            Top = area.Top + 12;
        }
        Topmost = _settings.Topmost;
        UpdatePinGlyph();
        _suppressSave = false;
        _settingsLoaded = true;

        if (_metrics.RamTotalGb > 0)
            RamDetail.Text = $"Totale {_metrics.RamTotalGb:0.0} GB";
        _slowTimer.Start();
        _procTimer.Start();
        _driveTimer.Start();
        BuildDriveList();
        _ = TickSlowAsync();
        _ = TickProcessesAsync();
    }

    private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _metrics.Updated -= OnMetricsUpdated;
        _slowTimer.Stop();
        _procTimer.Stop();
        _driveTimer.Stop();
        SaveGeometry();
        await Task.Yield();
    }

    private void OnMetricsUpdated(object? sender, EventArgs e)
    {
        Dispatcher.BeginInvoke(() =>
        {
            CpuValue.Text = $"{_metrics.Cpu:0.0}%";
            RamValue.Text = $"{_metrics.RamUsedPercent:0}%";
            RamUsed.Text = $"{_metrics.RamUsedGb:0.0} / {_metrics.RamTotalGb:0.0} GB";
            GpuValue.Text = $"{_metrics.Gpu:0}%";
            GpuVram.Text = BuildVramText();

            NetValue.Text = $"↓ {Fmt(_metrics.NetRecvBytesPerSec)}  ↑ {Fmt(_metrics.NetSentBytesPerSec)}";
            DiskValue.Text = $"↓ {Fmt(_metrics.DiskReadBytesPerSec)}  ↑ {Fmt(_metrics.DiskWriteBytesPerSec)}";

            CpuSpark.Values = _metrics.CpuHistory.ToArray();
            RamSpark.Values = _metrics.RamHistory.ToArray();
            GpuSpark.Values = _metrics.GpuHistory.ToArray();
            NetSpark.Values = _metrics.NetHistory.ToArray();
            DiskSpark.Values = _metrics.DiskHistory.ToArray();

            RamDetail.Text = BuildRamDetail();
            UptimeText.Text = BuildUptime();
        });
    }

    private string BuildPowerEstimate()
    {
        const double BaselineW = 30; // mobo + RAM + SSD + fans (rough)
        double gpuW = _hw.TotalGpuPowerW ?? _hw.GpuPowerW ?? 0;
        double cpuW;
        bool cpuFromLhm = _hw.CpuPowerW is double cw && cw > 0;
        if (cpuFromLhm)
        {
            cpuW = _hw.CpuPowerW!.Value;
        }
        else
        {
            // Stima: idle ~10W + 1.5W per 1% CPU (cap a ~170W per TDP tipico high-end)
            cpuW = Math.Min(170, 10 + _metrics.Cpu * 1.5);
        }

        double total = cpuW + gpuW + BaselineW;
        if (total <= 0) return "";
        var tilde = cpuFromLhm ? "" : "~";
        return $"PC {tilde}{total:0} W";
    }

    private string BuildVramText()
    {
        double totalGb = 0;
        double usedGb = 0;
        if (_hw.GpuMemTotalMb is double t && t > 0 && t <= 48 * 1024d) totalGb = t / 1024d;
        if (_hw.GpuMemUsedMb is double u && u > 0) usedGb = u / 1024d;

        // Sanity check
        if (totalGb > 0 && usedGb > totalGb) usedGb = 0;

        if (totalGb <= 0 && usedGb <= 0) return "";
        if (totalGb > 0 && usedGb > 0)
            return $"VRAM {usedGb:F1} / {totalGb:F1} GB";
        if (totalGb > 0) return $"VRAM ? / {totalGb:F1} GB";
        return $"VRAM {usedGb:F1} GB";
    }

    private string BuildRamDetail()
    {
        var parts = new List<string>();
        if (_metrics.RamTotalGb > 0) parts.Add($"Totale {_metrics.RamTotalGb:0.0}G");
        if (_metrics.RamAvailableGb > 0) parts.Add($"Disp {_metrics.RamAvailableGb:0.0}G");
        if (_metrics.RamCachedGb > 0) parts.Add($"Cache {_metrics.RamCachedGb:0.0}G");
        if (_metrics.RamCommittedGb > 0)
            parts.Add($"Commit {_metrics.RamCommittedGb:0.0}/{_metrics.RamCommitLimitGb:0.0}G");
        if (_metrics.PageFilePercent > 0)
            parts.Add($"PF {_metrics.PageFilePercent:0}%");
        return string.Join("  •  ", parts);
    }

    private string BuildUptime()
    {
        var up = DateTime.Now - _machineBoot;
        if (up.TotalDays >= 1) return $"uptime {(int)up.TotalDays}g {up.Hours}h {up.Minutes}m";
        if (up.TotalHours >= 1) return $"uptime {up.Hours}h {up.Minutes}m";
        return $"uptime {up.Minutes}m";
    }

    private async Task TickSlowAsync()
    {
        if (_slowRunning) return;
        _slowRunning = true;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            var swHw = System.Diagnostics.Stopwatch.StartNew();
            await Task.Run(() => _hw.Update());
            swHw.Stop();
            PerfLog.LogSlow("hw.Update", swHw.ElapsedMilliseconds);

            var swNet = System.Diagnostics.Stopwatch.StartNew();
            await _net.UpdateAsync();
            swNet.Stop();
            PerfLog.LogSlow("net.UpdateAsync", swNet.ElapsedMilliseconds);

            if (_hw.GpuLoadPercent is double gl) _metrics.GpuExternal = gl;
            if (_hw.GpuMemUsedMb is double gm && gm > 0) _metrics.VramUsedExternalGb = gm / 1024d;

            CpuClock.Text = _hw.CpuClockMhz is double mhz && mhz > 0 ? $"{mhz / 1000:0.00} GHz" : "";
            CpuPower.Text = _hw.CpuPowerW is double cpw && cpw > 0 ? $"{cpw:0.0}W" : "";

            if (_hw.CpuTempC is double t1 && t1 > 5)
            {
                CpuTemp.Text = _hw.CpuTempFromWmi ? $"{t1:0}°C*" : $"{t1:0}°C";
                CpuTemp.ToolTip = _hw.CpuTempFromWmi
                    ? "Temp da WMI Thermal Zone (approssimata).\nEsegui come admin per lettura MSR esatta."
                    : null;
            }
            else
            {
                CpuTemp.Text = "n/a";
                CpuTemp.ToolTip = "Temperatura CPU non disponibile.\nEsegui come amministratore per abilitarla.";
            }

            GpuTemp.Text = _hw.GpuTempC is double t2 && t2 > 5 ? $"{t2:0}°C" : "";
            DiskTemp.Text = _hw.StorageTempC is double t3 && t3 > 5 ? $"{t3:0}°C" : "";

            var gpuDetailParts = new List<string>();
            if (_hw.GpuClockMhz is double gc && gc > 0) gpuDetailParts.Add($"core {gc:0}MHz");
            if (_hw.GpuMemClockMhz is double mc && mc > 0) gpuDetailParts.Add($"mem {mc:0}MHz");
            if (_hw.GpuPowerW is double pw && pw > 0) gpuDetailParts.Add($"{pw:0.0}W");
            GpuDetail.Text = string.Join("  •  ", gpuDetailParts);

            NetPing.Text = _net.PingMs is double ms ? $"ping {ms:0} ms" : "";
            var netDetailParts = new List<string>();
            if (!string.IsNullOrEmpty(_net.AdapterType))
            {
                var s = _net.AdapterType;
                if (!string.IsNullOrEmpty(_net.Ssid)) s += $" • {_net.Ssid}";
                if (_net.SignalPercent is int sig) s += $" ({sig}%)";
                netDetailParts.Add(s);
            }
            if (!string.IsNullOrEmpty(_net.LocalIp)) netDetailParts.Add($"local {_net.LocalIp}");
            if (!string.IsNullOrEmpty(_net.PublicIp)) netDetailParts.Add($"pub {_net.PublicIp}");
            NetDetail.Text = string.Join("  •  ", netDetailParts);
            PowerText.Text = BuildPowerEstimate();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Slow tick failed: {ex.Message}");
        }
        finally
        {
            _slowRunning = false;
            sw.Stop();
            PerfLog.LogSlow("TickSlow total", sw.ElapsedMilliseconds, 500);
        }
    }

    private async Task TickProcessesAsync()
    {
        if (_procRunning) return;
        _procRunning = true;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        try
        {
            await Task.Run(() => _proc.Update());
            TopCpuList.ItemsSource = _proc.TopByCpu.Select(p => new ProcessRow
            {
                Name = p.Name,
                Value = $"{p.CpuPercent,5:0.0}%"
            }).ToArray();
            TopRamList.ItemsSource = _proc.TopByMemory.Select(p => new ProcessRow
            {
                Name = p.Name,
                Value = FmtRam(p.WorkingSetBytes)
            }).ToArray();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Proc tick failed: {ex.Message}");
        }
        finally
        {
            _procRunning = false;
            sw.Stop();
            PerfLog.LogSlow("TickProcesses", sw.ElapsedMilliseconds, 200);
        }
    }

    private async void BuildDriveList()
    {
        // DriveInfo.GetDrives può bloccare se un HDD sta dormendo. Offload su background.
        await Task.Run(() => _drive.Update());
        DriveList.ItemsSource = _drive.Drives.Select(d => new DriveRow
        {
            Name = d.Name,
            Caption = $"{FmtGb(d.FreeGb)} di {FmtGb(d.TotalGb)}",
            UsedPercent = d.UsedPercent
        }).ToArray();
    }

    private static string FmtGb(double gb)
    {
        if (gb >= 1000) return $"{gb / 1024d:0.0} TB";
        if (gb >= 100) return $"{gb:0} GB";
        return $"{gb:0.0} GB";
    }

    private static string Fmt(double bytesPerSec)
    {
        if (bytesPerSec >= 1024d * 1024 * 1024) return $"{bytesPerSec / 1024d / 1024 / 1024:0.00} GB/s";
        if (bytesPerSec >= 1024d * 1024) return $"{bytesPerSec / 1024d / 1024:0.0} MB/s";
        if (bytesPerSec >= 1024) return $"{bytesPerSec / 1024d:0} KB/s";
        return $"{bytesPerSec:0} B/s";
    }

    private static string FmtRam(long bytes)
    {
        if (bytes >= 1024L * 1024 * 1024) return $"{bytes / 1024d / 1024 / 1024:0.00} GB";
        if (bytes >= 1024L * 1024) return $"{bytes / 1024d / 1024:0} MB";
        return $"{bytes / 1024d:0} KB";
    }

    private void DragHandle_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            Topmost = !Topmost;
            _settings.Topmost = Topmost;
            UpdatePinGlyph();
            SettingsService.Save(_settings);
            return;
        }
        if (e.LeftButton == MouseButtonState.Pressed) DragMove();
    }

    private void PinButton_Click(object sender, RoutedEventArgs e)
    {
        Topmost = !Topmost;
        _settings.Topmost = Topmost;
        UpdatePinGlyph();
        SaveGeometry();
    }

    public void UpdatePinGlyph()
    {
        if (Topmost)
        {
            PinButton.Content = "";
            PinButton.Foreground = (System.Windows.Media.Brush)FindResource("CpuBrush");
            PinButton.Opacity = 1.0;
            PinButton.ToolTip = "Sempre in primo piano: ON (clicca per disattivare)";
        }
        else
        {
            PinButton.Content = "";
            PinButton.Foreground = (System.Windows.Media.Brush)FindResource("SubtleBrush");
            PinButton.Opacity = 0.85;
            PinButton.ToolTip = "Sempre in primo piano: OFF (clicca per attivare)";
        }
    }

    private void HideButton_Click(object sender, RoutedEventArgs e)
    {
        Hide();
        _settings.IsVisible = false;
        SaveGeometry();
    }

    public void ToggleVisible()
    {
        if (IsVisible) { Hide(); _settings.IsVisible = false; }
        else { Show(); Activate(); _settings.IsVisible = true; }
        SaveGeometry();
    }

    public void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
        _settings.IsVisible = true;
        SaveGeometry();
    }

    private void SaveGeometry()
    {
        if (_suppressSave) return;
        _settings.Left = Left;
        _settings.Top = Top;
        _settings.Width = Width;
        _settings.Height = Height;
        _settings.Topmost = Topmost;
        SettingsService.Save(_settings);
    }

    private void ClampToVisibleScreen()
    {
        var minX = SystemParameters.VirtualScreenLeft;
        var minY = SystemParameters.VirtualScreenTop;
        var maxX = minX + SystemParameters.VirtualScreenWidth - 50;
        var maxY = minY + SystemParameters.VirtualScreenHeight - 30;
        if (Left < minX) Left = minX + 10;
        if (Top < minY) Top = minY + 10;
        if (Left > maxX) Left = maxX - 40;
        if (Top > maxY) Top = maxY - 40;
    }

    private const int WM_NCLBUTTONDOWN = 0x00A1;
    private const int HTBOTTOMRIGHT = 17;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    private void ResizeGrip_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.LeftButton != MouseButtonState.Pressed) return;
        var hwnd = new WindowInteropHelper(this).Handle;
        ReleaseCapture();
        SendMessage(hwnd, WM_NCLBUTTONDOWN, (IntPtr)HTBOTTOMRIGHT, IntPtr.Zero);
        e.Handled = true;
    }
}

internal sealed class ProcessRow
{
    public string Name { get; init; } = string.Empty;
    public string Value { get; init; } = string.Empty;
}

internal sealed class DriveRow
{
    public string Name { get; init; } = string.Empty;
    public string Caption { get; init; } = string.Empty;
    public double UsedPercent { get; init; }
}
