// Pulse 2, il processo dei sensori. Il pannello gira senza diritti e legge da sé tutto quello che può; quello che vuole i diritti
// di amministratore sta qui: i registri della CPU (temperatura, consumo, frequenza di ogni core, con LibreHardwareMonitor e il
// driver PawnIO) e il campionamento dei core col kernel, per sapere quale programma lavora su quale core.
//
// Parla col pannello attraverso «Local\Pulse2.Shared», la memoria che il pannello crea (src\shared.h: le posizioni qui sotto sono
// le stesse). Finché il pannello non c'è, o non dà segni di vita da cinque secondi, non legge e non campiona niente.
//
//   --fake          prova: valori finti al posto dei sensori e del campionamento, per controllare il collegamento senza diritti
//   --seconds <n>   esce da solo dopo n secondi
//   --follow-panel  esce quando il pannello che ha visto si chiude (per chi lo avvia a mano: da amministratore solo un altro
//                   processo elevato potrebbe chiuderlo)
using System;
using System.IO.MemoryMappedFiles;
using System.Linq;
using System.Threading;
using LibreHardwareMonitor.Hardware;
using ResourceMonitor.Services;

namespace Pulse2.Sensors;

internal static class Program
{
    private const string MapName = @"Local\Pulse2.Shared";
    private const int Magic = 0x32534C50, Version = 1, Size = 4096;
    private const int PSeq = 8, PTick = 16, PCores = 24, PSlotCount = 28, PSlotName = 40, PSlotPid = 280;
    private const int SSeq = 1048, SPid = 1052, STick = 1056, SFlags = 1064, STemp = 1068, SWatt = 1072, SMhz = 1076, SShareCores = 1080, SShare = 1084;
    private const int Slots = CoreSampler.Slots, NameChars = 40, MaxPids = 64, MaxCores = 256;
    private const int FlagElevated = 1, FlagPawnIo = 2, FlagSampling = 4, FlagNoSampling = 8, FlagFake = 16;

    private static Computer? _computer;
    private static IHardware? _cpu;
    private static bool _sensorsFailed;

    private static int Main(string[] args)
    {
        bool fake = args.Contains("--fake"), follow = args.Contains("--follow-panel"), seenPanel = false;
        int seconds = 0;
        int at = Array.IndexOf(args, "--seconds");
        if (at >= 0 && at + 1 < args.Length) int.TryParse(args[at + 1], out seconds);

        using var mutex = new Mutex(true, "Pulse2.Sensors.SingleInstance", out bool first);
        if (!first) return 0;

        bool elevated = CoreSampler.CanRun(), pawnIo = PawnIoInstalled();
        if (elevated && !fake) CoreSampler.StopOrphan();       // una sessione rimasta aperta da un processo terminato di forza
        PerfLog.Note($"start pid={Environment.ProcessId} elevated={elevated} pawnio={pawnIo} fake={fake}");

        MemoryMappedFile? map = null;
        MemoryMappedViewAccessor? view = null;
        CoreSampler? sampler = null;
        bool samplingFailed = false;
        var tracked = new string?[Slots];
        var trackedPids = new int[Slots][];
        float temp = -1, watt = -1, mhz = -1;
        long started = Environment.TickCount64;
        try
        {
            for (long tick = 0; ; tick++)
            {
                if (seconds > 0 && Environment.TickCount64 - started > seconds * 1000L) break;
                try
                {
                    // la memoria la crea il pannello: finché non c'è si aspetta, senza leggere niente
                    if (view is null)
                    {
                        try { map = MemoryMappedFile.OpenExisting(MapName, MemoryMappedFileRights.ReadWrite); view = map.CreateViewAccessor(0, Size); }
                        catch (Exception) { map?.Dispose(); map = null; Thread.Sleep(2000); continue; }
                    }
                    bool panel = view.ReadInt32(0) == Magic && view.ReadInt32(4) == Version && Environment.TickCount64 - view.ReadInt64(PTick) < 5000;
                    if (!panel)
                    {
                        // il pannello è chiuso o fermo: niente sessione del kernel aperta per nessuno
                        sampler?.Dispose(); sampler = null;
                        if (follow && seenPanel) break;
                        Thread.Sleep(1000);
                        continue;
                    }
                    seenPanel = true;

                    // che cosa chiede il pannello: una lettura vale solo se il suo contatore è pari e non è cambiato nel frattempo
                    int seq = view.ReadInt32(PSeq);
                    if ((seq & 1) == 0)
                    {
                        int cores = Math.Clamp(view.ReadInt32(PCores), 1, MaxCores);
                        var names = new string?[Slots];
                        var pids = new int[Slots][];
                        for (int k = 0; k < Slots; k++)
                        {
                            var chars = new char[NameChars];
                            view.ReadArray(PSlotName + k * NameChars * 2, chars, 0, NameChars);
                            int end = Array.IndexOf(chars, '\0');
                            string name = new string(chars, 0, end < 0 ? NameChars : end);
                            int count = Math.Clamp(view.ReadInt32(PSlotCount + k * 4), 0, MaxPids);
                            names[k] = name.Length > 0 ? name : null;
                            pids[k] = new int[names[k] is null ? 0 : count];
                            for (int i = 0; i < pids[k].Length; i++) pids[k][i] = (int)view.ReadUInt32(PSlotPid + (k * MaxPids + i) * 4);
                        }
                        if (view.ReadInt32(PSeq) == seq)
                        {
                            tracked = names; trackedPids = pids;
                            bool wanted = names.Any(n => n is not null);
                            if (!fake && elevated && !samplingFailed)
                            {
                                // la sessione del kernel resta aperta solo finché c'è almeno un programma da seguire
                                if (wanted && sampler is null)
                                {
                                    sampler = CoreSampler.TryStart(cores, out int error);
                                    if (sampler is null) { samplingFailed = true; PerfLog.Note($"core sampling not available: error {error}"); }
                                }
                                else if (!wanted && sampler is not null) { sampler.Dispose(); sampler = null; }
                                sampler?.Track(tracked, trackedPids);
                            }

                            // i registri della CPU ogni due secondi, come Pulse 1.x
                            if (tick % 2 == 0)
                            {
                                if (fake) { temp = 55 + 12 * (float)Math.Sin(tick / 6.0); watt = 90; mhz = 5200; }
                                else ReadCpu(out temp, out watt, out mhz);
                            }

                            var shares = new byte[cores * Slots];
                            bool sampling = false;
                            if (fake)
                            {
                                // due core per programma, sempre gli stessi, per vedere i colori senza il kernel
                                sampling = wanted;
                                for (int k = 0; k < Slots; k++)
                                    if (names[k] is not null)
                                        for (int c = 0; c < 2; c++) { int core = (k * 4 + 1 + c) % cores; shares[core * Slots + k] = 200; }
                            }
                            else if (sampler is not null)
                            {
                                sampling = true;
                                double[] taken = sampler.TakeShares();
                                for (int i = 0; i < shares.Length && i < taken.Length; i++) shares[i] = (byte)Math.Clamp(taken[i] * 255 + 0.5, 0, 255);
                            }

                            int flags = (elevated ? FlagElevated : 0) | (pawnIo ? FlagPawnIo : 0) | (sampling ? FlagSampling : 0)
                                      | (samplingFailed ? FlagNoSampling : 0) | (fake ? FlagFake : 0);
                            view.Write(SSeq, view.ReadInt32(SSeq) + 1);          // dispari: la scrittura è in corso
                            view.Write(SPid, (uint)Environment.ProcessId);
                            view.Write(STick, Environment.TickCount64);
                            view.Write(SFlags, flags);
                            view.Write(STemp, temp); view.Write(SWatt, watt); view.Write(SMhz, mhz);
                            view.Write(SShareCores, cores);
                            view.WriteArray(SShare, shares, 0, shares.Length);
                            view.Write(SSeq, view.ReadInt32(SSeq) + 1);          // pari: si può leggere
                        }
                    }
                }
                catch (Exception ex) { PerfLog.Note($"tick failed: {ex.GetType().Name}: {ex.Message}"); }
                Thread.Sleep(1000);
            }
        }
        finally
        {
            sampler?.Dispose();
            try { _computer?.Close(); } catch { }
            view?.Dispose(); map?.Dispose();
            PerfLog.Note("exit");
        }
        return 0;
    }

    /// <summary>Temperatura, consumo del pacchetto e frequenza del core più veloce: gli stessi sensori, scelti allo stesso modo,
    /// di HardwareMonitor di Pulse 1.x. -1 per quello che non c'è (senza diritti o senza PawnIO, tutti e tre).</summary>
    private static void ReadCpu(out float temp, out float watt, out float mhz)
    {
        temp = watt = mhz = -1;
        if (_sensorsFailed) return;
        try
        {
            if (_computer is null)
            {
                _computer = new Computer { IsCpuEnabled = true };
                _computer.Open();
                _cpu = _computer.Hardware.FirstOrDefault(h => h.HardwareType == HardwareType.Cpu);
            }
            if (_cpu is null) return;
            _cpu.Update();
            float? t = null, w = null, m = null;
            foreach (var s in _cpu.Sensors)
            {
                if (s.Value is not float v) continue;
                if (s.SensorType == SensorType.Temperature)
                {
                    bool package = s.Name.Contains("Package", StringComparison.OrdinalIgnoreCase) || s.Name.Contains("Core (Tctl/Tdie)", StringComparison.OrdinalIgnoreCase);
                    if (package || t is null) t = v;
                }
                else if (s.SensorType == SensorType.Clock && s.Name.StartsWith("CPU Core", StringComparison.OrdinalIgnoreCase))
                {
                    if (m is null || v > m) m = v;
                }
                else if (s.SensorType == SensorType.Power && w is null)
                {
                    w = v;      // il primo: su AMD e Intel è il pacchetto
                }
            }
            // il pacchetto, se c'è con quel nome, vince sul primo trovato
            var packagePower = _cpu.Sensors.FirstOrDefault(s => s.SensorType == SensorType.Power && s.Value is not null &&
                (s.Name.Equals("Package", StringComparison.OrdinalIgnoreCase) || s.Name.Equals("CPU Package", StringComparison.OrdinalIgnoreCase) ||
                 s.Name.Contains("Package Power", StringComparison.OrdinalIgnoreCase)));
            if (packagePower?.Value is float pw) w = pw;
            temp = t ?? -1; watt = w ?? -1; mhz = m ?? -1;
        }
        catch (Exception ex)
        {
            _sensorsFailed = true;
            PerfLog.Note($"cpu sensors off: {ex.GetType().Name}: {ex.Message}");
        }
    }

    private static bool PawnIoInstalled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO");
            return key is not null;
        }
        catch { return false; }
    }
}
