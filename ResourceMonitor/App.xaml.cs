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
    private GpuWatch? _gpuWatch;
    private Timer? _gpuRestart;
    private readonly object _gpuLock = new();
    private bool _restarting;

    // ripartenze: dopo un cambio del driver video o un "Restart as administrator" (RestartArg), dal task "Pulse restart" dopo un
    // crash (AfterCrashArg). In entrambi i casi si aspetta che il processo di prima liberi l'istanza unica
    private const string RestartArg = "--restart";
    private const string AfterCrashArg = "--after-crash";

    // l'istanza unica: un solo Pulse per sessione (docs/ARCHITECTURE.md, "Single instance")
    private const string InstanceName = "Pulse.SingleInstance";

    // Pulse è solo in inglese: anche i numeri (punto decimale) e le parole che WPF e Reticle prendono dalla cultura
    private static readonly System.Globalization.CultureInfo English = System.Globalization.CultureInfo.GetCultureInfo("en-US");

    protected override void OnStartup(StartupEventArgs e)
    {
        System.Globalization.CultureInfo.DefaultThreadCurrentCulture = English;
        System.Globalization.CultureInfo.DefaultThreadCurrentUICulture = English;
        System.Globalization.CultureInfo.CurrentCulture = English;
        System.Globalization.CultureInfo.CurrentUICulture = English;
        FrameworkElement.LanguageProperty.OverrideMetadata(typeof(FrameworkElement),
            new FrameworkPropertyMetadata(System.Windows.Markup.XmlLanguage.GetLanguage(English.IetfLanguageTag)));
        base.OnStartup(e);

        bool afterCrash = Array.IndexOf(e.Args, AfterCrashArg) >= 0;
        bool isNew = TakeInstance();
        if (!isNew && _singleInstance is not null && (afterCrash || Array.IndexOf(e.Args, RestartArg) >= 0))
        {
            // ripartenza (cambio del driver video, elevazione, crash): il processo di prima può essere ancora in uscita
            try { isNew = _singleInstance.WaitOne(TimeSpan.FromSeconds(10)); }
            catch (AbandonedMutexException) { isNew = true; }
        }
        if (!isNew)
        {
            // c'è già un Pulse: si esce senza toccare l'istanza unica, che non è nostra. Prima OnExit la rilasciava lo stesso e il
            // secondo Pulse andava in crash all'uscita, un crash che il task "Pulse restart" avrebbe fatto ripartire all'infinito
            LeaveInstance();
            Shutdown();
            return;
        }
        if (afterCrash && !SettingsService.AllowCrashRestart())
        {
            // tre ripartenze dopo un crash in dieci minuti: Pulse va in crash appena parte, meglio fermarsi
            PerfLog.Note("crash restart refused: already 3 in the last 10 minutes");
            ReleaseInstance();
            Shutdown();
            return;
        }

        // partito senza diritti ma con il task "Pulse" da amministratore scelto all'installazione: si riparte da lì, senza UAC. Se
        // il Pulse elevato non compare si resta questo, senza diritti, come prima (Services/AdminStart.cs)
        var handOff = AdminStart.TryHandOff(e.Args, ReleaseInstance, InstanceName);
        if (handOff == AdminStart.Result.HandedOff || (handOff == AdminStart.Result.Failed && !TakeInstance()))
        {
            LeaveInstance();
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
        _gpuWatch = new GpuWatch();
        _gpuWatch.Changed += OnGpuChanged;
        _metrics.Start();

        _window = new MainWindow(_metrics, _hw, _proc, _net, _drive, _settings);
        if (!_settings.StartHidden && _settings.IsVisible)
            _window.Show();

        BuildTrayIcon();
        if (Array.IndexOf(e.Args, "--appearance") >= 0 || Array.IndexOf(e.Args, "--aspetto") >= 0) ShowAppearance();
    }

    // prende l'istanza unica: false se c'è già un Pulse. Il mutex di un Pulse elevato può negare l'accesso a un Pulse senza
    // diritti, e anche questo vuol dire che c'è
    private bool TakeInstance()
    {
        try
        {
            _singleInstance = new Mutex(true, InstanceName, out bool isNew);
            return isNew;
        }
        catch (UnauthorizedAccessException)
        {
            _singleInstance = null;
            return false;
        }
    }

    // libera l'istanza unica, che è di questo processo
    private void ReleaseInstance()
    {
        _singleInstance?.ReleaseMutex();
        _singleInstance?.Dispose();
        _singleInstance = null;
    }

    // chiude il riferimento all'istanza unica senza rilasciarla: non è di questo processo
    private void LeaveInstance()
    {
        _singleInstance?.Dispose();
        _singleInstance = null;
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
        var showItem = new MenuItem { Header = "Show / Hide" };
        showItem.Click += (_, _) => _window?.ToggleVisible();

        _pinItem = new MenuItem
        {
            Header = "Always on top",
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
            Header = "Start with Windows",
            IsCheckable = true,
            IsChecked = AutoStart.IsEnabled()
        };
        _autoStartItem.Click += (_, _) =>
        {
            AutoStart.Set(_autoStartItem.IsChecked);
            if (_settings != null) { _settings.AutoStart = _autoStartItem.IsChecked; SettingsService.Save(_settings); }
        };

        var appearanceItem = new MenuItem { Header = "Appearance..." };
        appearanceItem.Click += (_, _) => ShowAppearance();

        // tema scuro, chiaro o come Windows, a un clic dal tray; resta salvato con le altre impostazioni di Appearance
        var themeItem = new MenuItem { Header = "Theme" };
        var themes = new[] { ("Dark", Reticle.Wpf.ThemeVariant.Dark), ("Light", Reticle.Wpf.ThemeVariant.Light), ("Follow Windows", Reticle.Wpf.ThemeVariant.System) };
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
                Header = "Restart as administrator",
                ToolTip = "Enables the process colours in the core bars and, with the PawnIO driver installed, CPU temperature and power (MSR registers)"
            };
            adminItem.Click += (_, _) => RestartAsAdmin();
            menu.Items.Add(adminItem);
        }

        menu.Items.Add(new Separator());
        var exitItem = new MenuItem { Header = "Exit" };
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
            // RestartArg: il processo elevato aspetta che questo liberi l'istanza unica, invece di uscire subito
            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = exe,
                Arguments = RestartArg,
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
        _gpuWatch?.Dispose();
        _tray?.Dispose();
        _metrics?.Dispose();
        _hw?.Dispose();
        if (_window != null) _window.Close();
        Shutdown();
    }

    // un adattatore video è sparito o comparso: da subito niente più letture dei sensori, poi, cinque secondi dopo l'ultimo
    // avviso (un aggiornamento del driver ne manda più d'uno), Pulse riparte da capo
    private void OnGpuChanged()
    {
        _hw?.Pause();
        lock (_gpuLock)
        {
            _gpuRestart ??= new Timer(_ => Dispatcher.InvokeAsync(RestartAfterGpuChange));
            _gpuRestart.Change(TimeSpan.FromSeconds(5), Timeout.InfiniteTimeSpan);
        }
    }

    // NVML resta legato al driver con cui si è aperto: dopo un cambio non lo si riapre nello stesso processo, si riparte. Il
    // processo nuovo eredita i diritti di questo (elevato resta elevato) e la finestra torna dov'era (le impostazioni sono salvate
    // alla chiusura). LibreHardwareMonitor non si chiude: toccherebbe NVML del driver vecchio, lo chiude l'uscita del processo
    private void RestartAfterGpuChange()
    {
        var exe = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exe) || _restarting) return;
        _restarting = true;   // un avviso arrivato durante la ripartenza non ne fa partire un'altra
        PerfLog.Note("display adapter changed: restarting");
        _gpuWatch?.Dispose();
        _tray?.Dispose();
        _metrics?.Dispose();
        _window?.Close();
        ReleaseInstance();
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exe, RestartArg) { UseShellExecute = false }); }
        catch (Exception ex) { PerfLog.Note($"restart failed: {ex.Message}"); }
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        ReleaseInstance();
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
