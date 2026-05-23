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
            UpdateLocalAdapter();
            if (DateTime.UtcNow - _lastWifiCheck > TimeSpan.FromSeconds(8))
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
            var signalMatch = SignalRegex().Match(output);

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
}
