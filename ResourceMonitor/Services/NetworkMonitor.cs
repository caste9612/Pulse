using System;
using System.Diagnostics;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace ResourceMonitor.Services;

public sealed partial class NetworkMonitor
{
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(5) };
    private DateTime _lastPublicIpFetch = DateTime.MinValue;
    private DateTime _lastWifiCheck = DateTime.MinValue;

    // la scheda attiva (nome, tipo, IP locale) cambia di rado: si rilegge quando Windows avvisa che gli indirizzi sono cambiati
    // (cavo, Wi-Fi, VPN) e comunque ogni minuto. GetAllNetworkInterfaces enumera anche i filtri e le schede virtuali: 30 ms ogni
    // volta su un PC con 65 interfacce (Hyper-V, WSL, VPN), prima ogni 2 secondi (12/10/2026)
    private volatile bool _addressesChanged = true;
    private DateTime _lastAdapterScan = DateTime.MinValue;

    public NetworkMonitor()
    {
        NetworkChange.NetworkAddressChanged += (_, _) => _addressesChanged = true;
    }

    public string? LocalIp { get; private set; }
    public string? PublicIp { get; private set; }
    public string? AdapterName { get; private set; }
    public string? AdapterType { get; private set; }
    public string? Ssid { get; private set; }
    public int? SignalPercent { get; private set; }
    public double? PingMs { get; private set; }

    public async Task UpdateAsync()
    {
        // Sposta tutto il lavoro sincrono potenzialmente lento (netsh, enum interfaces) su un thread di background.
        await Task.Run(() =>
        {
            if (_addressesChanged || DateTime.UtcNow - _lastAdapterScan > TimeSpan.FromMinutes(1))
            {
                _addressesChanged = false;
                _lastAdapterScan = DateTime.UtcNow;
                UpdateLocalAdapter();
            }
            // nome della rete e segnale servono solo quando la connessione attiva è Wi-Fi: via cavo niente netsh (era un processo
            // ogni 8 secondi, 31 ms di CPU ciascuno, per niente); in Wi-Fi ogni 30 secondi, il nome cambia di rado
            if (AdapterType != "Wi-Fi")
            {
                Ssid = null;
                SignalPercent = null;
            }
            else if (DateTime.UtcNow - _lastWifiCheck > TimeSpan.FromSeconds(30))
            {
                _lastWifiCheck = DateTime.UtcNow;
                UpdateWifi();
            }
        });

        if (DateTime.UtcNow - _lastPublicIpFetch > TimeSpan.FromMinutes(10) || PublicIp is null)
        {
            _lastPublicIpFetch = DateTime.UtcNow;
            _ = Task.Run(UpdatePublicIpAsync);
        }

        await PingAsync();
    }

    private void UpdateLocalAdapter()
    {
        try
        {
            var iface = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up &&
                            n.NetworkInterfaceType != NetworkInterfaceType.Loopback &&
                            n.NetworkInterfaceType != NetworkInterfaceType.Tunnel)
                .OrderByDescending(n => n.GetIPProperties().GatewayAddresses
                    .Any(g => g.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork))
                .ThenByDescending(n => n.Speed)
                .FirstOrDefault();

            if (iface is null) { LocalIp = null; AdapterName = null; AdapterType = null; return; }

            AdapterName = iface.Name;
            AdapterType = iface.NetworkInterfaceType switch
            {
                NetworkInterfaceType.Wireless80211 => "Wi-Fi",
                NetworkInterfaceType.Ethernet => "Ethernet",
                NetworkInterfaceType.GigabitEthernet => "Ethernet",
                _ => iface.NetworkInterfaceType.ToString()
            };

            LocalIp = iface.GetIPProperties().UnicastAddresses
                .FirstOrDefault(u => u.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)?.Address.ToString();
        }
        catch (Exception ex) { Debug.WriteLine($"Local adapter: {ex.Message}"); }
    }

    private void UpdateWifi()
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "netsh",
                Arguments = "wlan show interfaces",
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                UseShellExecute = false,
                StandardOutputEncoding = System.Text.Encoding.UTF8
            };
            using var p = Process.Start(psi);
            if (p is null) return;
            var output = p.StandardOutput.ReadToEnd();
            p.WaitForExit(800);

            var ssidMatch = SsidRegex().Match(output);
            // netsh traduce le etichette ("Segnale" su un Windows in italiano): senza "Signal" il segnale è la riga che finisce con
            // una percentuale, l'unica dell'elenco
            var signalMatch = SignalRegex().Match(output);
            if (!signalMatch.Success) signalMatch = PercentLineRegex().Match(output);

            if (ssidMatch.Success)
            {
                var v = ssidMatch.Groups[1].Value.Trim();
                Ssid = string.IsNullOrEmpty(v) ? null : v;
            }
            else Ssid = null;

            if (signalMatch.Success && int.TryParse(signalMatch.Groups[1].Value, out var pct))
                SignalPercent = pct;
            else
                SignalPercent = null;
        }
        catch { Ssid = null; SignalPercent = null; }
    }

    private async Task UpdatePublicIpAsync()
    {
        try
        {
            var ip = await Http.GetStringAsync("https://api.ipify.org");
            PublicIp = ip.Trim();
        }
        catch { /* keep previous */ }
    }

    private async Task PingAsync()
    {
        try
        {
            using var ping = new Ping();
            var reply = await ping.SendPingAsync(IPAddress.Parse("8.8.8.8"), 1000);
            PingMs = reply.Status == IPStatus.Success ? reply.RoundtripTime : null;
        }
        catch { PingMs = null; }
    }

    [GeneratedRegex(@"^\s*SSID\s*:\s*(.+)$", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex SsidRegex();

    [GeneratedRegex(@"Signal\s*:\s*(\d+)\s*%", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex SignalRegex();

    [GeneratedRegex(@"^[^:\r\n]+:\s*(\d+)\s*%\s*$", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex PercentLineRegex();
}
