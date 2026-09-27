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

        _singleInstance = new Mutex(true, "Pulse.SingleInstance", out bool isNew);
        if (!isNew)
        {
            Shutdown();
            return;
        }

        // tema e impostazioni Aspetto di Reticle, salvati in %LocalAppData%\Pulse\reticle-theme.json
        Reticle.Wpf.ThemeManager.Current.Initialize(new Reticle.Wpf.JsonThemeSettingsStore("Pulse"));

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
        if (Array.IndexOf(e.Args, "--aspetto") >= 0) ShowAppearance();
    }

    private Views.AppearanceWindow? _appearance;

    private void ShowAppearance()
    {
        if (_appearance is not null) { _appearance.Activate(); return; }
        _appearance = new Views.AppearanceWindow();
        _appearance.Closed += (_, _) => _appearance = null;
        _appearance.Show();
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

        var appearanceItem = new MenuItem { Header = "Aspetto..." };
        appearanceItem.Click += (_, _) => ShowAppearance();

        // tema scuro, chiaro o come Windows, a un clic dal tray; resta salvato con le altre impostazioni Aspetto
        var themeItem = new MenuItem { Header = "Tema" };
        var themes = new[] { ("Scuro", Reticle.Wpf.ThemeVariant.Dark), ("Chiaro", Reticle.Wpf.ThemeVariant.Light), ("Come Windows", Reticle.Wpf.ThemeVariant.System) };
        foreach (var (label, variant) in themes)
        {
            var item = new MenuItem { Header = label, IsCheckable = true, IsChecked = Reticle.Wpf.ThemeManager.Current.Settings.Theme == variant };
            item.Click += (_, _) =>
            {
                var theme = Reticle.Wpf.ThemeManager.Current;
                theme.Apply(theme.Settings with { Theme = variant });
                theme.Save();
            };
            themeItem.Items.Add(item);
        }

        menu.Items.Add(showItem);
        menu.Items.Add(themeItem);
        menu.Items.Add(appearanceItem);
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
    private const string ValueName = "Pulse";

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
