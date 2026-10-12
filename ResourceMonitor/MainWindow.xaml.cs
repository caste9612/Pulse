using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using ResourceMonitor.Services;

namespace ResourceMonitor;

public partial class MainWindow : Reticle.Wpf.ReticleWindow
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
    private bool _cornerPickerOpen;
    private bool _ramTipOpen;

    // il processo di ogni colore nelle barre dei core (-1 = colore libero): fino a tre processi di Top CPU sopra il mezzo core,
    // ciascuno col suo colore finché non scende sotto un quarto di core, anche se cambia posto
    private readonly int[] _slotPids = { -1, -1, -1 };
    private const double TrackFromCores = 0.5, TrackUntilCores = 0.25;

    // margine dai bordi dello schermo negli angoli e nella posizione del primo avvio: lo spazio S3 di Reticle (12 unità),
    // lo stesso che il contenuto ha dentro la cornice
    private const double CornerPadding = Reticle.Tokens.Space.S3;
    private DateTime _appStart = DateTime.UtcNow;
    private DateTime _machineBoot;

    public MainWindow(MetricsService metrics, HardwareMonitor hw, ProcessMonitor proc,
                      NetworkMonitor net, DriveMonitor drive, AppSettings settings)
    {
        InitializeComponent();
        UpdateSeriesBrushes();
        Reticle.Wpf.ThemeManager.Current.Changed += (_, _) => UpdateSeriesBrushes();
        _metrics = metrics;
        _hw = hw;
        _proc = proc;
        _net = net;
        _drive = drive;
        _settings = settings;
        _metrics.Updated += OnMetricsUpdated;

        CoreBars.Cores = _metrics.CpuCorePhysical;
        CpuCoresText.ToolTip = CoreBars.ToolTip = BuildCoresTooltip();

        // il tooltip della RAM si rifà a ogni secondo, ma non mentre è aperto: chi lo sta leggendo non lo vede cambiare
        foreach (var host in new FrameworkElement[] { RamAvailableText, RamBar })
        {
            host.ToolTipOpening += (_, _) => _ramTipOpen = true;
            host.ToolTipClosing += (_, _) => _ramTipOpen = false;
        }

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

        // la scelta dell'angolo si chiude con Esc, con un clic altrove nel widget e quando il widget perde il fuoco
        PreviewMouseDown += (_, _) => { if (_cornerPickerOpen && !CornerPicker.IsMouseOver && !CornerButton.IsMouseOver) CloseCornerPicker(); };
        PreviewKeyDown += (_, e) => { if (_cornerPickerOpen && e.Key == Key.Escape) { CloseCornerPicker(); e.Handled = true; } };
        Deactivated += (_, _) => CloseCornerPicker();
    }

    // colori di serie fissi (RAM viola, temperature ambra, i tre processi seguiti nelle barre dei core): categorie, non stati,
    // quindi non vengono dal tema; sul chiaro quelli del tema scuro si leggono poco e passano a tonalità più scure. CPU, GPU, NET e
    // DSK seguono già i toni del tema.
    private void UpdateSeriesBrushes()
    {
        bool light = Reticle.Wpf.ThemeManager.Current.Palette.IsLight;
        Resources["RamBrush"] = Frozen(light ? System.Windows.Media.Color.FromRgb(0x7C, 0x3A, 0xED) : System.Windows.Media.Color.FromRgb(0xB9, 0x87, 0xF7));
        Resources["TempBrush"] = Frozen(light ? System.Windows.Media.Color.FromRgb(0xC2, 0x41, 0x0C) : System.Windows.Media.Color.FromRgb(0xFF, 0x8A, 0x1F));
        var processes = new System.Windows.Media.Brush[]
        {
            Frozen(light ? System.Windows.Media.Color.FromRgb(0xC2, 0x18, 0x5B) : System.Windows.Media.Color.FromRgb(0xFF, 0x6E, 0xB4)),
            Frozen(light ? System.Windows.Media.Color.FromRgb(0x5B, 0x8C, 0x00) : System.Windows.Media.Color.FromRgb(0xB8, 0xE5, 0x5C)),
            Frozen(light ? System.Windows.Media.Color.FromRgb(0x39, 0x49, 0xAB) : System.Windows.Media.Color.FromRgb(0x8C, 0x9E, 0xFF))
        };
        for (int i = 0; i < processes.Length; i++) Resources[$"ProcessBrush{i}"] = processes[i];
        CoreBars.ShareFills = processes;
    }

    private static System.Windows.Media.SolidColorBrush Frozen(System.Windows.Media.Color color)
    {
        var brush = new System.Windows.Media.SolidColorBrush(color);
        brush.Freeze();
        return brush;
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
            Left = area.Right - Width - CornerPadding;
            Top = area.Top + CornerPadding;
        }
        Topmost = _settings.Topmost;
        UpdatePinGlyph();
        _suppressSave = false;
        _settingsLoaded = true;

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
            CoreBars.Values = _metrics.CpuCores;
            CoreBars.Shares = _metrics.CpuCoreShares;
            CpuCoresText.Text = _metrics.CpuCores.Length > 0 ? $"≈ {_metrics.CpuCoresInUse:0.0} cores in use" : "";
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

            RamAvailableText.Text = $"{_metrics.RamAvailableGb:0.0} GB available";
            RamBar.Parts = double.IsNaN(_metrics.RamInUseGb)
                ? new[] { _metrics.RamUsedGb, 0, 0, _metrics.RamAvailableGb }   // senza le liste della memoria: usata e disponibile
                : new[] { _metrics.RamInUseGb, _metrics.RamModifiedGb, _metrics.RamStandbyGb, _metrics.RamFreeGb };
            if (!_ramTipOpen) RamAvailableText.ToolTip = RamBar.ToolTip = BuildRamTooltip();
            UptimeText.Text = BuildUptime();
        });
    }

    private string BuildCoresTooltip()
    {
        int logical = _metrics.CpuCorePhysical.Length;
        int physical = _metrics.CpuCorePhysical.Distinct().Count();
        var cores = physical < logical ? $"logical cores ({logical}, on {physical} physical cores)" : $"cores ({logical})";
        var bars = physical < logical ? "one bar per logical core, those of the same physical core side by side" : "one bar per core";
        var colors = _metrics.CoreSamplingDisabled ? ""
            : _metrics.CoreSamplingAvailable
                ? $"\nColours: the share of each core used by the Top CPU processes above half a core ({TrackFromCores * 100 / logical:0.0} %), " +
                  "up to three (the small square next to the name), sampled by the kernel 100 times a second, only while there is one; " +
                  "the rest in the CPU colour."
                : "\nTo see in colour which Top CPU processes use the cores, run Pulse as administrator.";
        return $"Sum of the loads of the {cores}: how many full cores the current work is worth.\n" +
               $"Under the graph, {bars}: many medium bars mean parallel work, a single full one a single thread." + colors;
    }

    private string BuildPowerEstimate()
    {
        const double BaselineW = 30; // mobo + RAM + SSD + fans (rough)
        // una GPU integrata consuma dentro il pacchetto della CPU: contarla di nuovo la metterebbe due volte
        double gpuW = _hw.GpuIntegrated ? 0 : _hw.TotalGpuPowerW ?? _hw.GpuPowerW ?? 0;
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

    // perché manca la temperatura della CPU: LibreHardwareMonitor la legge dai registri MSR, che vogliono Pulse da amministratore
    // e il driver PawnIO, da installare a parte (la libreria non ne porta uno suo)
    private static string CpuSensorsHint() => (HardwareMonitor.IsElevated, HardwareMonitor.PawnIoInstalled) switch
    {
        (true, true) => "CPU temperature not available: the processor's sensors could not be read.",
        (true, false) => "CPU temperature and power need the PawnIO driver.\nInstall it from pawnio.eu (or: winget install namazso.PawnIO), then restart Pulse.",
        (false, true) => "CPU temperature not available.\nRun Pulse as administrator to enable it.",
        (false, false) => "CPU temperature and power need the PawnIO driver and Pulse running as administrator.\nInstall PawnIO from pawnio.eu (or: winget install namazso.PawnIO)."
    };

    // "Intel(R) UHD Graphics 750" → "Intel UHD Graphics 750"
    private static string ShortName(string name) =>
        name.Replace("(R)", "", StringComparison.OrdinalIgnoreCase).Replace("(TM)", "", StringComparison.OrdinalIgnoreCase).Replace("  ", " ").Trim();

    private string BuildVramText()
    {
        double totalGb = 0;
        double usedGb = 0;
        // la memoria condivisa di un'integrata è la RAM che può prendere, metà di quella del PC: niente tetto dei 48 GB
        bool shared = _hw.GpuMemShared;
        if (_hw.GpuMemTotalMb is double t && t > 0 && (shared || t <= 48 * 1024d)) totalGb = t / 1024d;
        if (_hw.GpuMemUsedMb is double u && u > 0) usedGb = u / 1024d;

        // Sanity check
        if (totalGb > 0 && usedGb > totalGb) usedGb = 0;

        var label = shared ? "shared" : "VRAM";
        if (totalGb <= 0 && usedGb <= 0) return "";
        if (totalGb > 0 && usedGb > 0)
            return $"{label} {usedGb:F1} / {totalGb:F1} GB";
        if (totalGb > 0) return $"{label} ? / {totalGb:F1} GB";
        return $"{label} {usedGb:F1} GB";
    }

    // le parti della barra della RAM spiegate, poi la memoria impegnata e il file di paging (prima erano sigle nella riga)
    private string BuildRamTooltip()
    {
        var m = _metrics;
        var lines = new List<string>();
        if (!double.IsNaN(m.RamInUseGb))
        {
            lines.Add($"In use {m.RamInUseGb:0.0} GB: processes, drivers and the system.");
            lines.Add($"Modified {m.RamModifiedGb:0.0} GB: to be written to disk before it can be reused.");
            lines.Add($"Cached {m.RamStandbyGb:0.0} GB: data and programs kept ready, given back by Windows as soon as it's needed.");
            lines.Add($"Free {m.RamFreeGb:0.0} GB.");
            lines.Add($"Available {m.RamAvailableGb:0.0} GB: cached plus free, what programs can still take.");
        }
        else
        {
            lines.Add($"Used {m.RamUsedGb:0.0} GB, available {m.RamAvailableGb:0.0} GB.");
        }
        if (m.RamCommitLimitGb > 0)
            lines.Add($"Committed {m.RamCommittedGb:0.0} of {m.RamCommitLimitGb:0.0} GB: what programs have asked for, against the limit of RAM plus page file.");
        lines.Add(m.PageFileTotalGb > 0
            ? $"Page file: {m.PageFileUsedGb:0.00} GB in use of {m.PageFileTotalGb:0.0} GB."
            : "No page file.");
        return string.Join("\n", lines);
    }

    private string BuildUptime() => FormatUptime(DateTime.Now - _machineBoot);

    private static string FormatUptime(TimeSpan up)
    {
        if (up.TotalDays >= 1) return $"uptime {(int)up.TotalDays}d {up.Hours}h {up.Minutes}m";
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

            // la frequenza misurata (il core più veloce, o la velocità media dei core al lavoro); senza, quella che dice Windows
            bool measured = _hw.CpuClockMhz is double m && m > 0;
            double cpuMhz = measured ? _hw.CpuClockMhz!.Value : (_metrics.CpuFreqMhz > 0 ? _metrics.CpuFreqMhz : 0);
            CpuClock.Text = cpuMhz > 0 ? $"{cpuMhz / 1000:0.00} GHz" : "";
            CpuClock.ToolTip = !measured ? "Clock reported by Windows (the base clock on many processors)"
                : _hw.CpuClockIsAverage ? "Average speed of the cores at work, as in Task Manager"
                : "Clock of the fastest core";
            CpuPower.Text = _hw.CpuPowerW is double cpw && cpw > 0 ? $"{cpw:0.0}W" : "";

            if (_hw.CpuTempC is double t1 && t1 > 5)
            {
                CpuTemp.Text = $"{t1:0}°C";
                CpuTemp.ToolTip = null;
            }
            else
            {
                CpuTemp.Text = "n/a";
                CpuTemp.ToolTip = CpuSensorsHint();
            }

            GpuTemp.Text = _hw.GpuTempC is double t2 && t2 > 5 ? $"{t2:0}°C" : "";
            DiskTemp.Text = _hw.StorageTempC is double t3 && t3 > 5 ? $"{t3:0}°C" : "";

            var gpuDetailParts = new List<string>();
            if (_hw.GpuClockMhz is double gc && gc > 0) gpuDetailParts.Add($"core {gc:0}MHz");
            if (_hw.GpuMemClockMhz is double mc && mc > 0) gpuDetailParts.Add($"mem {mc:0}MHz");
            if (_hw.GpuPowerW is double pw && pw > 0) gpuDetailParts.Add($"{pw:0.0}W");
            // una GPU senza frequenze né consumo (l'integrata Intel senza PawnIO): al loro posto il nome, che dice quale si vede
            if (gpuDetailParts.Count == 0 && !string.IsNullOrEmpty(_hw.GpuName)) gpuDetailParts.Add(ShortName(_hw.GpuName));
            GpuDetail.Text = string.Join("  •  ", gpuDetailParts);
            GpuVram.ToolTip = _hw.GpuMemShared
                ? "Integrated GPU: it has no memory of its own and uses the RAM it shares with the CPU (Task Manager's shared GPU memory)"
                : null;

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
            var topCpu = _proc.TopByCpu;
            TrackLeaders(topCpu);
            // a schermo la percentuale della CPU; nel tooltip lo stesso valore in core, l'unità delle barre (su 32 core logici un
            // thread sempre al lavoro è il 3 %, cioè 1 core)
            int processors = Environment.ProcessorCount;
            TopCpuList.ItemsSource = topCpu.Select(p => new ProcessRow
            {
                Pid = p.Pid,
                Name = p.Name,
                Value = $"{p.CpuPercent,5:0.0}%",
                Tip = $"{p.CpuPercent * processors / 100:0.00} cores: how many full cores this process is worth",
                Slot = _metrics.CoreSamplingAvailable ? Array.IndexOf(_slotPids, p.Pid) : -2
            }).ToArray();
            TopRamList.ItemsSource = _proc.TopByMemory.Select(p => new ProcessRow
            {
                Pid = p.Pid,
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

    // nei colori delle barre dei core fino a tre processi di Top CPU: entra chi usa almeno mezzo core, esce chi scende sotto un
    // quarto (due soglie, così chi sta a cavallo non lampeggia); chi esce libera il suo colore, chi entra ne prende uno libero.
    // Il campionamento del kernel resta acceso solo finché c'è almeno un processo da seguire
    private void TrackLeaders(IReadOnlyList<ProcessSample> top)
    {
        if (!_metrics.CoreSamplingAvailable) return;
        int processors = Environment.ProcessorCount;
        var leaders = top
            .Where(p => p.CpuPercent * processors / 100 >= (Array.IndexOf(_slotPids, p.Pid) >= 0 ? TrackUntilCores : TrackFromCores))
            .Take(CoreSampler.Slots).Select(p => p.Pid).ToArray();
        for (int s = 0; s < _slotPids.Length; s++)
            if (Array.IndexOf(leaders, _slotPids[s]) < 0) _slotPids[s] = -1;
        foreach (int pid in leaders)
            if (Array.IndexOf(_slotPids, pid) < 0) _slotPids[Array.IndexOf(_slotPids, -1)] = pid;
        _metrics.TrackProcesses(_slotPids);
        _metrics.SetCoreSampling(leaders.Length > 0);
    }

    private async void BuildDriveList()
    {
        // DriveInfo.GetDrives può bloccare se un HDD sta dormendo. Offload su background.
        await Task.Run(() => _drive.Update());
        DriveList.ItemsSource = _drive.Drives.Select(d => new DriveRow
        {
            Name = d.Name,
            Caption = $"{FmtGb(d.FreeGb)} free of {FmtGb(d.TotalGb)}",
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
        // pin acceso con l'accento, spento in text.muted; i brush seguono il tema dal vivo
        if (Topmost)
        {
            PinButton.SetResourceReference(ForegroundProperty, "Reticle.Brush.Accent");
            PinButton.ToolTip = "Always on top: on (click to turn off)";
        }
        else
        {
            PinButton.SetResourceReference(ForegroundProperty, "Reticle.Brush.Text.Muted");
            PinButton.ToolTip = "Always on top: off (click to turn on)";
        }
    }

    private void CornerButton_Click(object sender, RoutedEventArgs e)
    {
        if (_cornerPickerOpen) CloseCornerPicker();
        else OpenCornerPicker();
    }

    // le frecce compaiono sotto il bottone e si aprono dal centro verso l'angolo che indicano: traslazione e dissolvenza con le
    // durate di Reticle, niente scala sui bottoni (01-bottone); con le animazioni di Windows spente compaiono e basta
    private void OpenCornerPicker()
    {
        _cornerPickerOpen = true;
        CornerButton.SetResourceReference(ForegroundProperty, "Reticle.Brush.Accent");
        CornerPicker.Visibility = Visibility.Visible;
        CornerPicker.IsHitTestVisible = true;
        CornerPicker.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var root = (FrameworkElement)CornerPicker.Parent;
        double width = CornerPicker.DesiredSize.Width;
        double center = CornerButton.TranslatePoint(new Point(CornerButton.ActualWidth / 2, 0), root).X;
        CornerPicker.Margin = new Thickness(Math.Clamp(center - width / 2, 0, Math.Max(0, root.ActualWidth - width)), 2, 0, 0);

        if (!Reticle.Wpf.ThemeManager.Current.Capabilities.AnimationsEnabled)
        {
            CornerPicker.BeginAnimation(OpacityProperty, null);
            return;
        }
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        CornerPicker.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, Reticle.Tokens.Motion.Base) { EasingFunction = ease });
        foreach (var arrow in new[] { CornerTopLeft, CornerTopRight, CornerBottomLeft, CornerBottomRight })
        {
            // partono tutte dal centro della tessera, mezza cella più in là, e si aprono verso il proprio angolo
            var (right, bottom) = CornerOf(arrow);
            double dx = arrow.DesiredSize.Width / 2, dy = arrow.DesiredSize.Height / 2;
            if (arrow.RenderTransform is not TranslateTransform move) arrow.RenderTransform = move = new TranslateTransform();
            move.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(right ? -dx : dx, 0, Reticle.Tokens.Motion.Reveal) { EasingFunction = ease });
            move.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(bottom ? -dy : dy, 0, Reticle.Tokens.Motion.Reveal) { EasingFunction = ease });
        }
    }

    private void CloseCornerPicker()
    {
        if (!_cornerPickerOpen) return;
        _cornerPickerOpen = false;
        CornerButton.ClearValue(ForegroundProperty);
        CornerPicker.IsHitTestVisible = false;
        if (!Reticle.Wpf.ThemeManager.Current.Capabilities.AnimationsEnabled)
        {
            CornerPicker.Visibility = Visibility.Collapsed;
            return;
        }
        var fade = new DoubleAnimation(0, Reticle.Tokens.Motion.Fast) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } };
        // riaperta durante la dissolvenza: resta a schermo
        fade.Completed += (_, _) => { if (!_cornerPickerOpen) CornerPicker.Visibility = Visibility.Collapsed; };
        CornerPicker.BeginAnimation(OpacityProperty, fade);
    }

    private void CornerArrow_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement arrow) return;
        CloseCornerPicker();
        var (right, bottom) = CornerOf(arrow);
        MoveToCorner(right, bottom);
    }

    private static (bool Right, bool Bottom) CornerOf(FrameworkElement arrow) =>
        arrow.Tag is string corner
            ? (corner.EndsWith("Right", StringComparison.Ordinal), corner.StartsWith("Bottom", StringComparison.Ordinal))
            : (false, false);

    // l'angolo dell'area di lavoro del monitor su cui sta il widget, quindi mai sopra la barra delle applicazioni; in pixel di
    // Windows, perché con monitor a scale diverse le coordinate WPF non bastano. La posizione nuova si salva da sola (LocationChanged)
    private void MoveToCorner(bool right, bool bottom)
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero || !GetWindowRect(hwnd, out var rect)) return;
        var info = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), ref info)) return;
        int pad = (int)Math.Round(CornerPadding * GetDpiForWindow(hwnd) / 96.0);
        var work = info.rcWork;
        int x = right ? work.Right - (rect.Right - rect.Left) - pad : work.Left + pad;
        int y = bottom ? work.Bottom - (rect.Bottom - rect.Top) - pad : work.Top + pad;
        SetWindowPos(hwnd, IntPtr.Zero, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // larghezze minime dei grafici quando il widget si adatta al contenuto: sparkline e barre restano leggibili
    private const double FitGraphWidth = 96, FitDriveBarWidth = 60;

    private void FitButton_Click(object sender, RoutedEventArgs e) => FitToContent();

    // la misura minima che mostra tutto, presa dal layout vero: in altezza esattamente il contenuto (niente barra di scorrimento,
    // niente spazio in fondo), in larghezza la più stretta in cui nessun valore è tagliato, nemmeno quando arriva al suo testo più
    // lungo, con i grafici alla loro larghezza minima. Le righe di Top CPU e Top RAM non contano: i nomi si accorciano con i puntini
    // per costruzione. Il widget resta attaccato al bordo destro o inferiore dello schermo se ci stava, e comunque dentro l'area di
    // lavoro. L'elenco dei dischi arriva qualche secondo dopo l'avvio (un disco che dorme): adattare prima lo lascerebbe fuori
    private void FitToContent()
    {
        var hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero || !GetWindowRect(hwnd, out var before)) return;
        var info = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        bool haveWork = GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), ref info);
        int near = (int)Math.Round(2 * CornerPadding * GetDpiForWindow(hwnd) / 96.0);
        bool right = haveWork && info.rcWork.Right - before.Right <= near;
        bool bottom = haveWork && info.rcWork.Bottom - before.Bottom <= near;

        // 1. l'altezza alla larghezza di adesso: dopo, la barra di scorrimento non c'è più e non ruba larghezza al calcolo
        FitHeight();

        // 2. la larghezza naturale di ogni riga con i grafici fermi al minimo: misurate senza limite di larghezza, le colonne * fanno
        // come le Auto e i grafici valgono la larghezza data qui solo per la misura (Width, non MinWidth: la barra dei dischi di
        // Reticle, senza limite, chiede più del minimo)
        var graphs = Graphs().ToList();
        foreach (var (element, min) in graphs) element.Width = min;
        // i valori che crescono si misurano col loro testo più lungo: una misura presa a traffico basso o subito dopo l'avvio non
        // taglia niente quando arrivano i MB/s o i giorni di uptime. Un valore che su questo PC non c'è (vuoto) resta vuoto
        double fast = 999.9 * 1024 * 1024;
        var widest = new (System.Windows.Controls.TextBlock Block, string Text)[]
        {
            (CpuValue, $"{100.0:0.0}%"), (RamValue, "100%"), (GpuValue, "100%"),
            (NetValue, $"↓ {Fmt(fast)}  ↑ {Fmt(fast)}"), (DiskValue, $"↓ {Fmt(fast)}  ↑ {Fmt(fast)}"),
            (NetPing, "ping 999 ms"), (UptimeText, FormatUptime(new TimeSpan(99, 23, 59, 0))), (PowerText, "PC ~999 W"),
            (CpuPower, $"{999.9:0.0}W"), (CpuTemp, "100°C"), (GpuTemp, "100°C"), (DiskTemp, "100°C")
        };
        var shown = widest.Select(w => w.Block.Text).ToArray();
        for (int i = 0; i < widest.Length; i++)
            if (!string.IsNullOrEmpty(shown[i])) widest[i].Block.Text = widest[i].Text;
        var unlimited = new Size(double.PositiveInfinity, double.PositiveInfinity);
        double rows = 0;
        foreach (UIElement child in ContentPanel.Children)
        {
            if (child == TopPanels) continue;
            child.Measure(unlimited);
            rows = Math.Max(rows, child.DesiredSize.Width);
        }
        HeaderInfo.Measure(unlimited);
        HeaderButtons.Measure(unlimited);
        foreach (var (element, _) in graphs) element.ClearValue(WidthProperty);
        for (int i = 0; i < widest.Length; i++) widest[i].Block.Text = shown[i];
        double header = HeaderInfo.DesiredSize.Width + HeaderButtons.DesiredSize.Width + DragHandle.Padding.Left + DragHandle.Padding.Right;
        // a ciascuna si aggiunge quello che la finestra ha intorno: cornice e margini per il contenuto (dalla larghezza del pannello
        // che scorre, che comprende la barra: WPF la toglie un passaggio dopo), cornice per l'intestazione
        double around = ActualWidth - ContentScroll.ActualWidth + ContentPanel.Margin.Left + ContentPanel.Margin.Right;
        double width = Math.Max(rows + around, header + ActualWidth - DragHandle.ActualWidth);
        Width = Math.Max(MinWidth, Math.Ceiling(width));
        FitHeight();

        // 3. al bordo a cui era attaccato, e dentro l'area di lavoro
        if (!haveWork || !GetWindowRect(hwnd, out var after)) return;
        int w = after.Right - after.Left, h = after.Bottom - after.Top;
        int x = right ? before.Right - w : after.Left;
        int y = bottom ? before.Bottom - h : after.Top;
        x = Math.Max(info.rcWork.Left, Math.Min(x, info.rcWork.Right - w));
        y = Math.Max(info.rcWork.Top, Math.Min(y, info.rcWork.Bottom - h));
        if (x != after.Left || y != after.Top)
            SetWindowPos(hwnd, IntPtr.Zero, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // l'altezza esatta del contenuto: quanto manca o avanza nel pannello che scorre; un pixel in più se l'arrotondamento lascia la barra
    private void FitHeight()
    {
        UpdateLayout();
        Height = Math.Max(MinHeight, Math.Ceiling(ActualHeight - ContentScroll.ViewportHeight + ContentScroll.ExtentHeight));
        UpdateLayout();
        for (int i = 0; i < 3 && ContentScroll.ExtentHeight > ContentScroll.ViewportHeight; i++)
        {
            Height += 1;
            UpdateLayout();
        }
    }

    // i grafici, che si accorciano quando il widget si stringe, con la larghezza minima che conservano adattando la misura
    private IEnumerable<(FrameworkElement Element, double Min)> Graphs()
    {
        foreach (var graph in new FrameworkElement[] { CpuSpark, RamSpark, GpuSpark, NetSpark, DiskSpark, CoreBars, RamBar })
            yield return (graph, FitGraphWidth);
        foreach (var bar in Descendants<System.Windows.Controls.ProgressBar>(DriveList))
            yield return (bar, FitDriveBarWidth);
    }

    private static IEnumerable<T> Descendants<T>(DependencyObject root) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match) yield return match;
            foreach (var deeper in Descendants<T>(child)) yield return deeper;
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

    private static ProcessRow? ExtractRow(object sender) =>
        sender is System.Windows.Controls.MenuItem mi ? mi.CommandParameter as ProcessRow : null;

    private void ProcOpenFolder_Click(object sender, RoutedEventArgs e)
    {
        var row = ExtractRow(sender);
        if (row is null) return;
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(row.Pid);
            var path = p.MainModule?.FileName;
            if (!string.IsNullOrEmpty(path))
                System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{path}\"");
        }
        catch { /* Access denied for system processes, ignore */ }
    }

    private void ProcProperties_Click(object sender, RoutedEventArgs e)
    {
        var row = ExtractRow(sender);
        if (row is null) return;
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(row.Pid);
            var path = p.MainModule?.FileName;
            if (string.IsNullOrEmpty(path)) return;
            var info = new SHELLEXECUTEINFO
            {
                cbSize = Marshal.SizeOf<SHELLEXECUTEINFO>(),
                lpVerb = "properties",
                lpFile = path,
                nShow = 1,
                fMask = 0x0000000C // SEE_MASK_INVOKEIDLIST | SEE_MASK_NOCLOSEPROCESS
            };
            ShellExecuteEx(ref info);
        }
        catch { }
    }

    private void ProcKill_Click(object sender, RoutedEventArgs e)
    {
        var row = ExtractRow(sender);
        if (row is null) return;
        var result = MessageBox.Show(
            $"End {row.Name} (PID {row.Pid})?",
            "Confirm",
            MessageBoxButton.YesNo,
            MessageBoxImage.Warning);
        if (result != MessageBoxResult.Yes) return;
        try
        {
            using var p = System.Diagnostics.Process.GetProcessById(row.Pid);
            p.Kill();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Couldn't end the process: {ex.Message}", "Error",
                MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private void ProcSearch_Click(object sender, RoutedEventArgs e)
    {
        var row = ExtractRow(sender);
        if (row is null) return;
        var url = $"https://www.google.com/search?q={Uri.EscapeDataString(row.Name + " process windows")}";
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url)
            {
                UseShellExecute = true
            });
        }
        catch { }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct SHELLEXECUTEINFO
    {
        public int cbSize;
        public uint fMask;
        public IntPtr hwnd;
        [MarshalAs(UnmanagedType.LPTStr)] public string lpVerb;
        [MarshalAs(UnmanagedType.LPTStr)] public string lpFile;
        [MarshalAs(UnmanagedType.LPTStr)] public string? lpParameters;
        [MarshalAs(UnmanagedType.LPTStr)] public string? lpDirectory;
        public int nShow;
        public IntPtr hInstApp;
        public IntPtr lpIDList;
        [MarshalAs(UnmanagedType.LPTStr)] public string? lpClass;
        public IntPtr hkeyClass;
        public uint dwHotKey;
        public IntPtr hIcon;
        public IntPtr hProcess;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern bool ShellExecuteEx(ref SHELLEXECUTEINFO lpExecInfo);

    private const int WM_NCLBUTTONDOWN = 0x00A1;
    private const int HTBOTTOMRIGHT = 17;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    private const uint MONITOR_DEFAULTTONEAREST = 2;
    private const uint SWP_NOSIZE = 0x0001, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [DllImport("user32.dll")]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

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
    public int Pid { get; init; }
    public string Name { get; init; } = string.Empty;
    public string Value { get; init; } = string.Empty;
    public string? Tip { get; init; }
    /// <summary>Solo Top CPU: il colore del processo nelle barre dei core (0–2), -1 se non è seguito, -2 senza campionamento.</summary>
    public int Slot { get; init; } = -2;
}

internal sealed class DriveRow
{
    public string Name { get; init; } = string.Empty;
    public string Caption { get; init; } = string.Empty;
    public double UsedPercent { get; init; }
}
