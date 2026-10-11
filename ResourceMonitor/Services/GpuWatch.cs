using System;
using System.Runtime.InteropServices;

namespace ResourceMonitor.Services;

/// <summary>
/// Avvisa quando un adattatore video sparisce o compare: un aggiornamento del driver (anche da Windows Update), un reset dopo un
/// blocco, una scheda tolta. Il 10/10/2026 un aggiornamento del driver NVIDIA ha mandato in crash Pulse quattro secondi dopo,
/// alla prima lettura di NVML col driver vecchio. Notifiche di CM_Register_Notification sull'interfaccia degli adattatori video:
/// nessuna finestra, nessun costo finché non succede niente. <see cref="Changed"/> arriva su un thread del pool.
/// </summary>
public sealed class GpuWatch : IDisposable
{
    // GUID_DEVINTERFACE_DISPLAY_ADAPTER
    private static readonly Guid DisplayAdapter = new("5b45201d-f2f2-4f3b-85bb-30ff1f953599");

    private readonly NotifyCallback _callback;   // tenuto vivo: lo chiama Windows da codice nativo
    private IntPtr _registration;

    public event Action? Changed;

    public GpuWatch()
    {
        _callback = OnNotify;
        var filter = new CM_NOTIFY_FILTER
        {
            cbSize = Marshal.SizeOf<CM_NOTIFY_FILTER>(),
            FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE,
            ClassGuid = DisplayAdapter
        };
        int result = CM_Register_Notification(ref filter, IntPtr.Zero, _callback, out _registration);
        if (result != 0)
        {
            _registration = IntPtr.Zero;
            PerfLog.Note($"gpu watch not started: CM_Register_Notification error {result}");
        }
    }

    private int OnNotify(IntPtr notify, IntPtr context, int action, IntPtr data, int size)
    {
        if (action is CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL or CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        {
            try { Changed?.Invoke(); } catch { }   // un'eccezione non deve tornare nel codice nativo
        }
        return 0;
    }

    public void Dispose()
    {
        if (_registration == IntPtr.Zero) return;
        CM_Unregister_Notification(_registration);
        _registration = IntPtr.Zero;
    }

    private const int CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE = 0;
    private const int CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL = 0;
    private const int CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL = 1;

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int NotifyCallback(IntPtr notify, IntPtr context, int action, IntPtr data, int size);

    // CM_NOTIFY_FILTER: cbSize, Flags, FilterType, Reserved, poi l'unione (la più grande è InstanceId, 200 WCHAR): 416 byte
    [StructLayout(LayoutKind.Explicit, Size = 416)]
    private struct CM_NOTIFY_FILTER
    {
        [FieldOffset(0)] public int cbSize;
        [FieldOffset(4)] public int Flags;
        [FieldOffset(8)] public int FilterType;
        [FieldOffset(12)] public int Reserved;
        [FieldOffset(16)] public Guid ClassGuid;
    }

    [DllImport("cfgmgr32.dll")]
    private static extern int CM_Register_Notification(ref CM_NOTIFY_FILTER filter, IntPtr context, NotifyCallback callback, out IntPtr registration);

    [DllImport("cfgmgr32.dll")]
    private static extern int CM_Unregister_Notification(IntPtr registration);
}
