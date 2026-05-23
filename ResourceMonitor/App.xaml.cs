using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Forms;
using ResourceMonitor.Services;
using Application = System.Windows.Application;
using MessageBox = System.Windows.MessageBox;

namespace ResourceMonitor;

public partial class App : Application
{
    private MetricsService? _metrics;
    private HardwareMonitor? _hw;
    private ProcessMonitor? _proc;
    private NetworkMonitor? _net;
    private DriveMonitor? _drive;
    private AppSettings? _settings;
    private MainWindow? _window;
    private NotifyIcon? _tray;
    private Mutex? _singleInstance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        _singleInstance = new Mutex(true, "ResourceMonitor.SingleInstance", out bool isNew);
        if (!isNew)
        {
            Shutdown();
            return;
        }

        _settings = SettingsService.Load();
        _metrics = new MetricsService();
        _hw = new HardwareMonitor();
        _proc = new ProcessMonitor();
        _net = new NetworkMonitor();
        _drive = new DriveMonitor();
        _metrics.Start();

        _window = new MainWindow(_metrics, _hw, _proc, _net, _drive, _settings);
        if (!_settings.StartHidden && _settings.IsVisible)
            _window.Show();

        BuildTrayIcon();
    }

    private void BuildTrayIcon()
    {
        var menu = new ContextMenuStrip();

        var showItem = new ToolStripMenuItem("Mostra / Nascondi", null, (_, _) => _window?.ToggleVisible());
        var pinItem = new ToolStripMenuItem("Sempre in primo piano")
        {
            CheckOnClick = true,
            Checked = _settings?.Topmost ?? true
        };
        pinItem.CheckedChanged += (_, _) =>
        {
            if (_window != null && _settings != null)
            {
                _window.Topmost = pinItem.Checked;
                _window.UpdatePinGlyph();
                _settings.Topmost = pinItem.Checked;
                SettingsService.Save(_settings);
            }
        };

        var autoStartItem = new ToolStripMenuItem("Avvia con Windows")
        {
            CheckOnClick = true,
            Checked = AutoStart.IsEnabled()
        };
        autoStartItem.CheckedChanged += (_, _) =>
        {
            AutoStart.Set(autoStartItem.Checked);
            if (_settings != null) { _settings.AutoStart = autoStartItem.Checked; SettingsService.Save(_settings); }
        };

        var exitItem = new ToolStripMenuItem("Esci", null, (_, _) => ExitApp());

        menu.Items.Add(showItem);
        menu.Items.Add(pinItem);
        menu.Items.Add(autoStartItem);

        if (!IsAdmin())
        {
            menu.Items.Add(new ToolStripSeparator());
            var adminItem = new ToolStripMenuItem("Riavvia come amministratore",
                null,
                (_, _) => RestartAsAdmin());
            adminItem.ToolTipText = "Abilita lettura temp/watt CPU (registri MSR)";
            menu.Items.Add(adminItem);
        }

        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(exitItem);

        _tray = new NotifyIcon
        {
            Icon = CreateTrayIcon(),
            Visible = true,
            Text = "Resource Monitor",
            ContextMenuStrip = menu
        };
        _tray.MouseClick += (_, args) =>
        {
            if (args.Button == MouseButtons.Left)
                _window?.ToggleVisible();
        };
        _tray.DoubleClick += (_, _) => _window?.ShowFromTray();
    }

    private static Icon CreateTrayIcon()
    {
        const int size = 32;
        using var bmp = new Bitmap(size, size);
        using (var g = Graphics.FromImage(bmp))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(Color.Transparent);
            using var bg = new SolidBrush(Color.FromArgb(230, 27, 27, 35));
            g.FillEllipse(bg, 1, 1, size - 2, size - 2);
            using var pen = new Pen(Color.FromArgb(255, 111, 207, 232), 2.6f) { LineJoin = LineJoin.Round };
            var pts = new[]
            {
                new PointF(6, 22), new PointF(11, 14), new PointF(16, 19),
                new PointF(21, 9), new PointF(26, 16)
            };
            g.DrawLines(pen, pts);
        }
        var hIcon = bmp.GetHicon();
        try { return Icon.FromHandle(hIcon).Clone() as Icon ?? SystemIcons.Application; }
        finally { DestroyIcon(hIcon); }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr handle);

    private static bool IsAdmin()
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            var principal = new System.Security.Principal.WindowsPrincipal(identity);
            return principal.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch { return false; }
    }

    private void RestartAsAdmin()
    {
        try
        {
            var exe = Environment.ProcessPath;
            if (string.IsNullOrEmpty(exe)) return;
            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = exe,
                UseShellExecute = true,
                Verb = "runas"
            };
            System.Diagnostics.Process.Start(psi);
            ExitApp();
        }
        catch
        {
            // utente ha rifiutato UAC
        }
    }

    private void ExitApp()
    {
        if (_tray != null) { _tray.Visible = false; _tray.Dispose(); }
        _metrics?.Dispose();
        _hw?.Dispose();
        if (_window != null) _window.Close();
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _singleInstance?.ReleaseMutex();
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}

internal static class AutoStart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "ResourceMonitor";

    public static bool IsEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is string s && !string.IsNullOrWhiteSpace(s);
        }
        catch { return false; }
    }

    public static void Set(bool enabled)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RunKey, true);
            if (key is null) return;
            if (enabled)
            {
                var exe = Environment.ProcessPath ?? throw new InvalidOperationException();
                key.SetValue(ValueName, $"\"{exe}\"");
            }
            else
            {
                key.DeleteValue(ValueName, false);
            }
        }
        catch { }
    }
}
