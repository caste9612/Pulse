using System;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using ResourceMonitor.Services;

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
    private TrayIcon? _tray;
    private Mutex? _singleInstance;
    private MenuItem? _pinItem;
    private MenuItem? _autoStartItem;

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
        _tray = new TrayIcon { Tooltip = "Pulse" };
        _tray.LeftClicked += () => _window?.ToggleVisible();
        _tray.DoubleClicked += () => _window?.ShowFromTray();
        _tray.BuildMenu = PopulateMenu;
        _tray.Initialize();
    }

    private void PopulateMenu(ContextMenu menu)
    {
        var showItem = new MenuItem { Header = "Mostra / Nascondi" };
        showItem.Click += (_, _) => _window?.ToggleVisible();

        _pinItem = new MenuItem
        {
            Header = "Sempre in primo piano",
            IsCheckable = true,
            IsChecked = _settings?.Topmost ?? true
        };
        _pinItem.Click += (_, _) =>
        {
            if (_window != null && _settings != null)
            {
                _window.Topmost = _pinItem.IsChecked;
                _window.UpdatePinGlyph();
                _settings.Topmost = _pinItem.IsChecked;
                SettingsService.Save(_settings);
            }
        };

        _autoStartItem = new MenuItem
        {
            Header = "Avvia con Windows",
            IsCheckable = true,
            IsChecked = AutoStart.IsEnabled()
        };
        _autoStartItem.Click += (_, _) =>
        {
            AutoStart.Set(_autoStartItem.IsChecked);
            if (_settings != null) { _settings.AutoStart = _autoStartItem.IsChecked; SettingsService.Save(_settings); }
        };

        menu.Items.Add(showItem);
        menu.Items.Add(_pinItem);
        menu.Items.Add(_autoStartItem);

        if (!IsAdmin())
        {
            menu.Items.Add(new Separator());
            var adminItem = new MenuItem
            {
                Header = "Riavvia come amministratore",
                ToolTip = "Abilita lettura temp/watt CPU (registri MSR)"
            };
            adminItem.Click += (_, _) => RestartAsAdmin();
            menu.Items.Add(adminItem);
        }

        menu.Items.Add(new Separator());
        var exitItem = new MenuItem { Header = "Esci" };
        exitItem.Click += (_, _) => ExitApp();
        menu.Items.Add(exitItem);
    }

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
        catch { /* user denied UAC */ }
    }

    private void ExitApp()
    {
        _tray?.Dispose();
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
