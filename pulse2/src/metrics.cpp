// Le letture veloci, una volta al secondo sul thread della finestra: CPU e ogni core, memoria, disco, rete. Chiamate dirette a
// Windows, le stesse di Pulse 1.x (NativeMetrics.cs); nessuna vuole i diritti di amministratore. Una lettura che fallisce lascia il
// valore di prima: il pannello non mostra mai un numero inventato.
#include <winsock2.h>
#include <ws2ipdef.h>
#include "app.h"
#include <winternl.h>
#include <iphlpapi.h>

static void Push(float* h, float v) { for (int i = 0; i < HIST - 1; i++) h[i] = h[i + 1]; h[HIST - 1] = v; }

Nice NiceScale(float m)
{
    static const wchar_t* unit[3] = { L" KB/s", L" MB/s", L" GB/s" };
    static const int step[9] = { 1, 2, 5, 10, 20, 50, 100, 200, 500 };
    const float lowest = 100 * 1024.f, highest = 10 * 1073741824.f;
    Nice best = { highest, L"10 GB/s" };
    float base = 1024;
    for (int u = 0; u < 3; u++, base *= 1024)
        for (int k = 0; k < 9; k++) {
            float max = step[k] * base;
            if (max < lowest || max > highest || max < m) continue;
            Nice n; n.max = max; Sb b; b.i(step[k]).t(unit[u]); lstrcpynW(n.label, b.s, 16);
            return n;
        }
    return best;
}

// ---- CPU: i tempi di ogni processore logico (NtQuerySystemInformation, classe 8) ----
struct ProcPerf { LARGE_INTEGER idle, kernel, user, dpc, interrupt; ULONG interruptCount; };   // 48 byte; kernel comprende idle
static ProcPerf g_prevCpu[MAXCORE];
static int g_prevCores;

static void ReadCpu(State& s)
{
    static ProcPerf cur[MAXCORE];
    ULONG len = 0;
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)8, cur, sizeof(cur), &len) < 0) return;
    int n = (int)(len / sizeof(ProcPerf));
    if (n <= 0 || n > MAXCORE) return;
    if (n == g_prevCores) {
        float sum = 0;
        for (int i = 0; i < n; i++) {
            LONGLONG idle = cur[i].idle.QuadPart - g_prevCpu[i].idle.QuadPart;
            LONGLONG total = (cur[i].kernel.QuadPart + cur[i].user.QuadPart) - (g_prevCpu[i].kernel.QuadPart + g_prevCpu[i].user.QuadPart);
            float load = total > 0 ? Clampf(1 - (float)idle / (float)total, 0, 1) : 0;
            s.cores[i] = load; sum += load;
        }
        s.ncore = n; s.coresInUse = sum; s.cpu = sum / n * 100;
    }
    CopyMemory(g_prevCpu, cur, n * sizeof(ProcPerf)); g_prevCores = n;
}

// quali processori logici stanno sullo stesso core fisico: una volta, all'avvio
static void ReadTopology(State& s)
{
    for (int i = 0; i < MAXCORE; i++) s.phys[i] = (unsigned char)(i & 255);
    DWORD len = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return;
    BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, len);
    if (!buf) return;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf, &len)) {
        int core = 0;
        for (DWORD off = 0; off < len; core++) {
            auto* e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf + off);
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount > 0 && e->Processor.GroupMask[0].Group == 0)
                for (int b = 0; b < 64; b++) if (e->Processor.GroupMask[0].Mask & ((KAFFINITY)1 << b)) s.phys[b] = (unsigned char)(core & 255);
            if (!e->Size) break;
            off += e->Size;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ---- memoria: totale e disponibile, e le liste delle pagine per la barra (in uso, modificata, in cache, libera) ----
struct MemList { SIZE_T zero, free, modified, modifiedNoWrite, bad, byPriority[8], repurposed[8], modifiedPageFile; };
struct PageFile { ULONG next, total, inUse, peak; UNICODE_STRING name; };               // in pagine (classe 18)

static void ReadRam(State& s)
{
    const float GB = 1073741824.f;
    MEMORYSTATUSEX m = { sizeof(m) };
    if (!GlobalMemoryStatusEx(&m) || !m.ullTotalPhys) return;
    s.ramTotal = m.ullTotalPhys / GB; s.ramAvail = m.ullAvailPhys / GB; s.ramUsed = s.ramTotal - s.ramAvail; s.ramPct = s.ramUsed / s.ramTotal * 100;
    MemList ml; ULONG len = 0;
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)80, &ml, sizeof(ml), &len) >= 0) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        float page = si.dwPageSize / GB; SIZE_T standby = 0;
        for (int i = 0; i < 8; i++) standby += ml.byPriority[i];
        s.ramFree = (ml.zero + ml.free) * page; s.ramModified = ml.modified * page; s.ramStandby = standby * page;
        s.ramInUse = s.ramTotal - s.ramFree - s.ramModified - s.ramStandby; if (s.ramInUse < 0) s.ramInUse = 0;
        s.ramParts = true;
    } else s.ramParts = false;                                                       // senza le liste: la barra mostra usata e disponibile
    // per il tooltip: quanta memoria i programmi hanno chiesto contro il limite (RAM più file di paging), e il file di paging vero
    s.commitLimit = m.ullTotalPageFile / GB; s.commit = (m.ullTotalPageFile - m.ullAvailPageFile) / GB;
    static BYTE pf[4096]; s.pageTotal = s.pageUsed = 0;
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)18, pf, sizeof(pf), &len) >= 0 && len >= sizeof(PageFile)) {
        SYSTEM_INFO si; GetSystemInfo(&si); float page = si.dwPageSize / GB;
        for (ULONG off = 0; off + sizeof(PageFile) <= len;) {
            const PageFile* e = (const PageFile*)(pf + off);
            s.pageTotal += e->total * page; s.pageUsed += e->inUse * page;
            if (!e->next) break;
            off += e->next;
        }
    }
}

// ---- disco: i byte letti e scritti da tutto il sistema (NtQuerySystemInformation, classe 2) ----
static LONGLONG g_prevRead, g_prevWrite; static bool g_haveIo;

static void ReadDisk(State& s, float seconds)
{
    static BYTE buf[1024]; ULONG len = 0;
    LONG st = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)2, buf, sizeof(buf), &len);
    if (st < 0 && len && len <= sizeof(buf)) st = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)2, buf, len, &len);
    if (st < 0) return;
    const LARGE_INTEGER* v = (const LARGE_INTEGER*)buf;                              // tempo di idle, poi byte letti, scritti, altri
    LONGLONG rd = v[1].QuadPart, wr = v[2].QuadPart;
    if (g_haveIo && seconds > 0) {
        s.dr = rd >= g_prevRead ? (float)(rd - g_prevRead) / seconds : 0;
        s.dw = wr >= g_prevWrite ? (float)(wr - g_prevWrite) / seconds : 0;
    }
    g_prevRead = rd; g_prevWrite = wr; g_haveIo = true;
}

// ---- rete: solo le schede fisiche accese (i filtri WFP e QoS conterebbero lo stesso traffico più volte). Le schede si scelgono
// ogni dieci secondi dalla tabella intera, e ogni secondo si leggono solo quelle ----
enum { MAXNIC = 16 };
static NET_LUID g_nic[MAXNIC]; static ULONG64 g_nicIn[MAXNIC], g_nicOut[MAXNIC]; static int g_nnic; static int g_nicAge = 1000;

static void ChooseNics()
{
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) != NO_ERROR || !t) return;
    NET_LUID nic[MAXNIC]; ULONG64 in[MAXNIC], out[MAXNIC]; int n = 0;
    for (ULONG i = 0; i < t->NumEntries && n < MAXNIC; i++) {
        const MIB_IF_ROW2& r = t->Table[i];
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        nic[n] = r.InterfaceLuid; in[n] = r.InOctets; out[n] = r.OutOctets;
        // una scheda già seguita tiene i suoi contatori di prima, così il secondo in corso non si perde
        for (int k = 0; k < g_nnic; k++) if (g_nic[k].Value == r.InterfaceLuid.Value) { in[n] = g_nicIn[k]; out[n] = g_nicOut[k]; }
        n++;
    }
    FreeMibTable(t);
    CopyMemory(g_nic, nic, sizeof(nic)); CopyMemory(g_nicIn, in, sizeof(in)); CopyMemory(g_nicOut, out, sizeof(out)); g_nnic = n; g_nicAge = 0;
}

static void ReadNet(State& s, float seconds)
{
    if (++g_nicAge >= 10) { bool first = g_nicAge >= 1000; ChooseNics(); if (first) return; }
    ULONG64 din = 0, dout = 0; bool lost = false;
    for (int i = 0; i < g_nnic; i++) {
        MIB_IF_ROW2 r; ZeroMemory(&r, sizeof(r)); r.InterfaceLuid = g_nic[i];
        if (GetIfEntry2(&r) != NO_ERROR || r.OperStatus != IfOperStatusUp) { lost = true; continue; }
        if (r.InOctets >= g_nicIn[i]) din += r.InOctets - g_nicIn[i];
        if (r.OutOctets >= g_nicOut[i]) dout += r.OutOctets - g_nicOut[i];
        g_nicIn[i] = r.InOctets; g_nicOut[i] = r.OutOctets;
    }
    if (seconds > 0) { s.down = (float)din / seconds; s.up = (float)dout / seconds; }
    if (lost) g_nicAge = 1000;                                                       // una scheda è sparita: si risceglie al prossimo secondo
}

// ---- il secondo ----
static LARGE_INTEGER g_freq, g_last;

void MetricsInit(State& s)
{
    ZeroMemory(&s, sizeof(s));
    s.cpuTemp = s.cpuWatt = s.gpuTemp = s.gpuWatt = s.pingMs = NONE;
    for (int i = 0; i < HIST; i++) s.hCt[i] = s.hGt[i] = NONE;
    ReadTopology(s);
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_last);
    ReadCpu(s); ReadRam(s); ReadDisk(s, 0); ReadNet(s, 0);                           // la prima lettura dà solo il punto di partenza
    s.uptimeSec = GetTickCount64() / 1000;
}

void MetricsTick(State& s)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float seconds = (float)((double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart); g_last = now;
    if (seconds < .05f) return;
    ReadCpu(s); ReadRam(s); ReadDisk(s, seconds); ReadNet(s, seconds);
    s.uptimeSec = GetTickCount64() / 1000;
    Push(s.hCpu, s.cpu); Push(s.hRam, s.ramPct); Push(s.hGpu, s.gpuOk ? s.gpu : 0);
    Push(s.hCt, s.cpuTemp); Push(s.hGt, s.gpuOk ? s.gpuTemp : NONE);
    Push(s.hDown, s.down); Push(s.hUp, s.up); Push(s.hDr, s.dr); Push(s.hDw, s.dw);
}
