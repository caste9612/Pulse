using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace ResourceMonitor.Services;

/// <summary>
/// Tray icon tramite P/Invoke Shell_NotifyIcon. Niente WinForms.
/// </summary>
public sealed class TrayIcon : IDisposable
{
    private const int WM_TRAYICON = 0x8001; // WM_APP + 1
    private const int WM_LBUTTONUP = 0x0202;
    private const int WM_LBUTTONDBLCLK = 0x0203;
    private const int WM_RBUTTONUP = 0x0205;
    private const int WM_CONTEXTMENU = 0x007B;

    private const uint NIM_ADD = 0;
    private const uint NIM_MODIFY = 1;
    private const uint NIM_DELETE = 2;
    private const uint NIM_SETVERSION = 4;

    private const uint NIF_MESSAGE = 0x01;
    private const uint NIF_ICON = 0x02;
    private const uint NIF_TIP = 0x04;

    private const int IDI_APPLICATION = 32512;
    private const int IDI_INFORMATION = 32516;
    private const int IMAGE_ICON = 1;
    private const int LR_SHARED = 0x00008000;
    private const int LR_DEFAULTSIZE = 0x00000040;

    private HwndSource? _msgWindow;
    private NOTIFYICONDATAW _data;
    private bool _added;
    private ContextMenu? _contextMenu;
    private IntPtr _hIcon;

    public event Action? LeftClicked;
    public event Action? DoubleClicked;
    public Action<ContextMenu>? BuildMenu { get; set; }
    public string Tooltip { get; set; } = "Pulse";

    public void Initialize()
    {
        var parameters = new HwndSourceParameters("PulseTrayMsgWindow")
        {
            ParentWindow = new IntPtr(-3), // HWND_MESSAGE
            Width = 0,
            Height = 0
        };
        _msgWindow = new HwndSource(parameters);
        _msgWindow.AddHook(WndProc);

        _hIcon = LoadAppIcon();

        _data = new NOTIFYICONDATAW
        {
            cbSize = (uint)Marshal.SizeOf<NOTIFYICONDATAW>(),
            hWnd = _msgWindow.Handle,
            uID = 1,
            uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
            uCallbackMessage = WM_TRAYICON,
            hIcon = _hIcon,
            szTip = Tooltip
        };

        if (Shell_NotifyIcon(NIM_ADD, ref _data))
        {
            _added = true;
            _data.uVersion = 4; // NOTIFYICON_VERSION_4
            Shell_NotifyIcon(NIM_SETVERSION, ref _data);
        }
    }

    private static IntPtr LoadAppIcon()
    {
        // Tentativo 1: estrai prima icona dell'eseguibile (quella settata via ApplicationIcon in csproj)
        var exe = Environment.ProcessPath;
        if (!string.IsNullOrEmpty(exe))
        {
            var large = new IntPtr[1];
            var small = new IntPtr[1];
            if (ExtractIconEx(exe, 0, large, small, 1) > 0 && small[0] != IntPtr.Zero)
            {
                if (large[0] != IntPtr.Zero) DestroyIcon(large[0]);
                return small[0];
            }
            if (large[0] != IntPtr.Zero) return large[0];
        }
        // Fallback: icona generica Windows
        return LoadImage(IntPtr.Zero, (IntPtr)IDI_APPLICATION, IMAGE_ICON, 16, 16, LR_SHARED);
    }

    public void UpdateTooltip(string tooltip)
    {
        Tooltip = tooltip;
        if (!_added) return;
        _data.szTip = tooltip;
        _data.uFlags = NIF_TIP;
        Shell_NotifyIcon(NIM_MODIFY, ref _data);
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WM_TRAYICON) return IntPtr.Zero;

        // Con NOTIFYICON_VERSION_4: LOWORD(lParam) = evento, HIWORD(lParam) = icon ID
        int evt = (int)((uint)lParam.ToInt64() & 0xFFFF);
        switch (evt)
        {
            case WM_LBUTTONUP:
                LeftClicked?.Invoke();
                handled = true;
                break;
            case WM_LBUTTONDBLCLK:
                DoubleClicked?.Invoke();
                handled = true;
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                ShowMenu();
                handled = true;
                break;
        }
        return IntPtr.Zero;
    }

    private void ShowMenu()
    {
        if (BuildMenu is null) return;
        _contextMenu ??= new ContextMenu
        {
            Background = new SolidColorBrush(Color.FromArgb(0xF0, 0x1E, 0x1E, 0x26)),
            Foreground = new SolidColorBrush(Color.FromArgb(0xFF, 0xF5, 0xF5, 0xF7)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(0x40, 0xFF, 0xFF, 0xFF)),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(2)
        };
        _contextMenu.Items.Clear();
        BuildMenu.Invoke(_contextMenu);

        // Necessario per chiudere automaticamente il menu se l'utente clicca fuori
        SetForegroundWindow(_msgWindow!.Handle);
        _contextMenu.Placement = PlacementMode.MousePoint;
        _contextMenu.IsOpen = true;
    }

    public void Dispose()
    {
        if (_added)
        {
            Shell_NotifyIcon(NIM_DELETE, ref _data);
            _added = false;
        }
        if (_hIcon != IntPtr.Zero)
        {
            DestroyIcon(_hIcon);
            _hIcon = IntPtr.Zero;
        }
        _msgWindow?.Dispose();
        _msgWindow = null;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern bool Shell_NotifyIcon(uint dwMessage, ref NOTIFYICONDATAW lpData);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int ExtractIconEx(string lpszFile, int nIconIndex,
        IntPtr[] phiconLarge, IntPtr[] phiconSmall, int nIcons);

    [DllImport("user32.dll")]
    private static extern IntPtr LoadImage(IntPtr hInst, IntPtr name, int type, int cx, int cy, int fuLoad);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 8)]
    private struct NOTIFYICONDATAW
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uID;
        public uint uFlags;
        public uint uCallbackMessage;
        public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string szTip;
        public uint dwState;
        public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string szInfo;
        public uint uVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string szInfoTitle;
        public uint dwInfoFlags;
        public Guid guidItem;
        public IntPtr hBalloonIcon;
    }
}
