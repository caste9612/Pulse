// Le letture lente, su un thread a parte perché possono aspettare (un disco che dorme, un ping, la rete): GPU, frequenza della CPU,
// scheda di rete e ping ogni 2 secondi, programmi ogni 5, dischi ogni 30, indirizzo pubblico ogni 10 minuti. Il thread scrive in una
// sua copia, protetta da una sezione critica; il pannello la prende a ogni secondo con SlowCopy. Niente qui vuole i diritti.
#include <winsock2.h>
#include <ws2ipdef.h>
#include "app.h"
#include <winternl.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <winhttp.h>
#include <powrprof.h>
#include <perflib.h>

struct SlowData {
    bool gpuOk; float gpu, gpuTemp, gpuWatt, vramUsed, vramTotal, gpuClock, gpuMemClock; wchar_t gpuName[64];
    float cpuGhz; int clockKind;
    wchar_t adapter[16], localIp[48], publicIp[48]; float pingMs;
    Drive drives[MAXDRIVE]; int ndrive;
    Program topCpu[NTOP], topRam[NTOP]; int nTopCpu, nTopRam;
};
static SlowData g_data;                  // quello che il pannello copia
static CRITICAL_SECTION g_lock;
static HANDLE g_thread, g_stop;

// ---------------------------------------------------------------------------------------------------------------------------
// Programmi: una sola chiamata dà tempi e memoria di tutti i processi (NtQuerySystemInformation, classe 5), anche di quelli
// che un Pulse senza diritti non può aprire. I processi con lo stesso nome si sommano.
// ---------------------------------------------------------------------------------------------------------------------------
struct ProcInfo {                        // la parte di SYSTEM_PROCESS_INFORMATION che serve, alle sue posizioni (x64)
    ULONG next, threads; LARGE_INTEGER wsPrivate; ULONG hardFaults, threadsHigh; ULONGLONG cycles;
    LARGE_INTEGER create, user, kernel; UNICODE_STRING image; LONG basePriority; HANDLE pid, parent;
    ULONG handles, session; ULONG_PTR key; SIZE_T peakVirtual, virtualSize; ULONG pageFaults; SIZE_T peakWs, ws;
};
enum { MAXPROC = 4096, MAXGROUP = 1024 };
struct Seen { DWORD pid; LONGLONG create, cpu; };
static Seen g_seen[MAXPROC]; static int g_nseen; static ULONGLONG g_seenAt;
struct Group { wchar_t name[PROGNAME]; int count; float cpu, ramMb; DWORD pid; float pidCpu, pidRam; };
static Group g_group[MAXGROUP];
static BYTE* g_procBuf; static ULONG g_procCap;

static ULONGLONG NowTicks() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void Tops(const Group* g, int n, bool byRam, Program* out, int& count)
{
    static bool used[MAXGROUP];
    for (int i = 0; i < n; i++) used[i] = false;
    count = 0;
    for (int k = 0; k < NTOP; k++) {
        int best = -1; float bv = -1;
        for (int i = 0; i < n; i++) { float v = byRam ? g[i].ramMb : g[i].cpu; if (!used[i] && v > bv) { bv = v; best = i; } }
        if (best < 0) break;
        used[best] = true;
        Program& p = out[count++];
        lstrcpynW(p.name, g[best].name, PROGNAME); p.count = g[best].count; p.cpu = g[best].cpu; p.ramMb = g[best].ramMb; p.pid = g[best].pid; p.slot = -2; p.npid = 0;
    }
}

static void ReadPrograms(SlowData& d)
{
    ULONG len = 0; LONG st = -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!g_procBuf) { g_procCap = g_procCap ? g_procCap : 512 * 1024; g_procBuf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, g_procCap); if (!g_procBuf) return; }
        st = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)5, g_procBuf, g_procCap, &len);
        if (st >= 0) break;
        HeapFree(GetProcessHeap(), 0, g_procBuf); g_procBuf = nullptr;                   // troppo piccolo: Windows ha detto quanto serve
        g_procCap = (len > g_procCap ? len : g_procCap) + 128 * 1024;
    }
    if (st < 0) return;

    ULONGLONG now = NowTicks(); double wall = g_seenAt ? (double)(now - g_seenAt) : 0;
    SYSTEM_INFO si; GetSystemInfo(&si); int ncpu = (int)si.dwNumberOfProcessors; if (ncpu < 1) ncpu = 1;
    static Seen cur[MAXPROC]; int ncur = 0, ngroup = 0;
    for (ULONG off = 0;;) {
        const ProcInfo* p = (const ProcInfo*)(g_procBuf + off);
        DWORD pid = (DWORD)(ULONG_PTR)p->pid;
        if (pid != 0 && p->image.Buffer && p->image.Length) {                            // il pid 0 è il tempo di idle, non un programma
            LONGLONG cpu = p->user.QuadPart + p->kernel.QuadPart; float pct = 0;
            if (wall > 0) for (int i = 0; i < g_nseen; i++) if (g_seen[i].pid == pid) {
                // lo stesso numero con un'altra data di nascita è un altro processo: parte da zero
                if (g_seen[i].create == p->create.QuadPart && cpu >= g_seen[i].cpu) pct = (float)((double)(cpu - g_seen[i].cpu) / wall / ncpu * 100);
                break;
            }
            if (ncur < MAXPROC) { cur[ncur].pid = pid; cur[ncur].create = p->create.QuadPart; cur[ncur].cpu = cpu; ncur++; }

            wchar_t name[PROGNAME]; int n = p->image.Length / 2; if (n > PROGNAME - 1) n = PROGNAME - 1;
            CopyMemory(name, p->image.Buffer, n * 2); name[n] = 0;
            if (n > 4 && !lstrcmpiW(name + n - 4, L".exe")) name[n - 4] = 0;              // come li chiama Gestione attività nei dettagli, senza estensione
            float ram = p->ws / 1048576.f;
            int g = 0; while (g < ngroup && lstrcmpiW(g_group[g].name, name)) g++;        // i nomi dei processi non distinguono maiuscole e minuscole
            if (g == ngroup) {
                if (ngroup == MAXGROUP) goto next;
                lstrcpyW(g_group[g].name, name); g_group[g].count = 0; g_group[g].cpu = 0; g_group[g].ramMb = 0; g_group[g].pid = pid; g_group[g].pidCpu = -1; g_group[g].pidRam = -1;
                ngroup++;
            }
            g_group[g].count++; g_group[g].cpu += pct; g_group[g].ramMb += ram;
            if (pct > g_group[g].pidCpu || (pct == g_group[g].pidCpu && ram > g_group[g].pidRam)) { g_group[g].pid = pid; g_group[g].pidCpu = pct; g_group[g].pidRam = ram; }
        }
    next:
        if (!p->next) break;
        off += p->next;
        if (off + sizeof(ProcInfo) > len) break;
    }
    CopyMemory(g_seen, cur, ncur * sizeof(Seen)); g_nseen = ncur; g_seenAt = now;
    Tops(g_group, ngroup, false, d.topCpu, d.nTopCpu);
    Tops(g_group, ngroup, true, d.topRam, d.nTopRam);
    // i processi di ogni programma di Top CPU: servono al campionamento dei core, che conta i campioni per processo
    for (ULONG off = 0;;) {
        const ProcInfo* p = (const ProcInfo*)(g_procBuf + off);
        if (p->pid && p->image.Buffer && p->image.Length) {
            wchar_t name[PROGNAME]; int n = p->image.Length / 2; if (n > PROGNAME - 1) n = PROGNAME - 1;
            CopyMemory(name, p->image.Buffer, n * 2); name[n] = 0;
            if (n > 4 && !lstrcmpiW(name + n - 4, L".exe")) name[n - 4] = 0;
            for (int k = 0; k < d.nTopCpu; k++) if (!lstrcmpiW(d.topCpu[k].name, name)) { if (d.topCpu[k].npid < MAXPID) d.topCpu[k].pids[d.topCpu[k].npid++] = (DWORD)(ULONG_PTR)p->pid; break; }
        }
        if (!p->next) break;
        off += p->next;
        if (off + sizeof(ProcInfo) > len) break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// GPU NVIDIA: la libreria del driver (NVML), caricata solo dalla cartella di sistema. Dopo un aggiornamento del driver una chiamata
// con la vecchia libreria può far cadere il processo (successo a Pulse 1.2 il 10/10/2026): qui ogni lettura sta in un __try, e al
// primo guasto NVML si lascia stare fino al riavvio.
// ---------------------------------------------------------------------------------------------------------------------------
struct NvUtil { unsigned gpu, memory; };
struct NvMem { unsigned long long total, free, used; };
typedef int (*NvVoid)(); typedef int (*NvCount)(unsigned*); typedef int (*NvHandle)(unsigned, void**);
typedef int (*NvUtilFn)(void*, NvUtil*); typedef int (*NvMemFn)(void*, NvMem*); typedef int (*NvUintFn)(void*, unsigned*);
typedef int (*NvTypedFn)(void*, int, unsigned*); typedef int (*NvNameFn)(void*, char*, unsigned);
static struct { HMODULE dll; void* dev; bool dead, tried; NvVoid shutdown; NvUtilFn util; NvMemFn mem; NvUintFn power; NvTypedFn temp, clock; char name[96]; } g_nv;

static bool NvOpenUnsafe()
{
    g_nv.dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_nv.dll) return false;
    NvVoid init = (NvVoid)GetProcAddress(g_nv.dll, "nvmlInit_v2");
    NvCount count = (NvCount)GetProcAddress(g_nv.dll, "nvmlDeviceGetCount_v2");
    NvHandle handle = (NvHandle)GetProcAddress(g_nv.dll, "nvmlDeviceGetHandleByIndex_v2");
    NvNameFn name = (NvNameFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetName");
    g_nv.shutdown = (NvVoid)GetProcAddress(g_nv.dll, "nvmlShutdown");
    g_nv.util = (NvUtilFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetUtilizationRates");
    g_nv.mem = (NvMemFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetMemoryInfo");
    g_nv.power = (NvUintFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetPowerUsage");
    g_nv.temp = (NvTypedFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetTemperature");
    g_nv.clock = (NvTypedFn)GetProcAddress(g_nv.dll, "nvmlDeviceGetClockInfo");
    if (!init || !count || !handle || !g_nv.util || !g_nv.mem || init() != 0) return false;
    unsigned n = 0; if (count(&n) != 0 || !n) return false;
    unsigned long long best = 0;                                                         // la scheda con più memoria, come Pulse 1.x
    for (unsigned i = 0; i < n; i++) {
        void* dev = nullptr; NvMem m = {};
        if (handle(i, &dev) != 0 || !dev || g_nv.mem(dev, &m) != 0) continue;
        if (m.total > best) { best = m.total; g_nv.dev = dev; }
    }
    if (!g_nv.dev) return false;
    if (name) name(g_nv.dev, g_nv.name, sizeof(g_nv.name));
    return true;
}
static bool NvReadUnsafe(SlowData& d)
{
    NvUtil u = {}; NvMem m = {}; unsigned v = 0;
    if (g_nv.util(g_nv.dev, &u) != 0 || g_nv.mem(g_nv.dev, &m) != 0) return false;
    d.gpu = (float)u.gpu; d.vramUsed = m.used / 1073741824.f; d.vramTotal = m.total / 1073741824.f;
    d.gpuTemp = g_nv.temp && g_nv.temp(g_nv.dev, 0, &v) == 0 ? (float)v : NONE;
    d.gpuWatt = g_nv.power && g_nv.power(g_nv.dev, &v) == 0 ? v / 1000.f : NONE;
    d.gpuClock = g_nv.clock && g_nv.clock(g_nv.dev, 0, &v) == 0 ? (float)v : 0;          // 0 = grafica, 2 = memoria
    d.gpuMemClock = g_nv.clock && g_nv.clock(g_nv.dev, 2, &v) == 0 ? (float)v : 0;
    return true;
}
static void ReadGpu(SlowData& d)
{
    if (!g_nv.tried && GetEnvironmentVariableW(L"PULSE_NO_NVML", nullptr, 0)) g_nv.dead = true;   // come in Pulse 1.x: per provare senza NVML
    if (g_nv.dead) { d.gpuOk = false; return; }
    bool ok = false;
    __try {
        if (!g_nv.tried) { g_nv.tried = true; if (!NvOpenUnsafe()) g_nv.dead = true; }
        if (!g_nv.dead) ok = NvReadUnsafe(d);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_nv.dead = true; LogLine("NVML: a call crashed, GPU readings off until restart"); }
    if (!ok && !g_nv.dead && !g_nv.dev) g_nv.dead = true;
    d.gpuOk = ok;
    if (ok && !d.gpuName[0]) MultiByteToWideChar(CP_UTF8, 0, g_nv.name, -1, d.gpuName, 64);
}

// ---------------------------------------------------------------------------------------------------------------------------
// frequenza della CPU, dischi, scheda di rete, ping, indirizzo pubblico
// ---------------------------------------------------------------------------------------------------------------------------
struct PowerInfo { ULONG number, maxMhz, currentMhz, mhzLimit, maxIdle, currentIdle; };

// La velocità media della CPU come la mostra Gestione attività: la frequenza nominale per «% Processor Performance» del counterset
// «Processor Information» del kernel, letto con l'API consumer di PerfLib V2 (advapi32), senza PDH: come CpuSpeed.cs di Pulse 1.3.1.
// Serve perché CallNtPowerInformation su molti processori dà sempre la frequenza nominale (4,50 GHz fissi sul 7950X di riferimento).
static const GUID kProcInfo = { 0xb4fc721a, 0x0378, 0x476f, { 0x89, 0xba, 0xa5, 0xa7, 0x9f, 0x81, 0x0b, 0x36 } };
static struct { HANDLE query; bool tried, primed; ULONGLONG prevPerf, prevBase; } g_speed;

static BYTE* SpeedReg(PerfRegInfoType type, DWORD& size)
{
    size = 0; PerfQueryCounterSetRegistrationInfo(nullptr, &kProcInfo, type, 0, nullptr, 0, &size);
    if (!size) return nullptr;
    BYTE* b = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (b && PerfQueryCounterSetRegistrationInfo(nullptr, &kProcInfo, type, 0, b, size, &size) != ERROR_SUCCESS) { HeapFree(GetProcessHeap(), 0, b); b = nullptr; }
    return b;
}
static bool SpeedAdd(HANDLE query, DWORD counter)
{
    struct { PERF_COUNTER_IDENTIFIER id; wchar_t name[8]; } c; ZeroMemory(&c, sizeof(c));   // l'identificatore e, subito dopo, il nome dell'istanza
    c.id.CounterSetGuid = kProcInfo; c.id.Size = sizeof(c); c.id.CounterId = counter; c.id.InstanceId = 0xFFFFFFFF; lstrcpyW(c.name, L"_Total");
    return PerfAddCounters(query, &c.id, sizeof(c)) == ERROR_SUCCESS && c.id.Status == ERROR_SUCCESS;
}
static bool SpeedOpen()
{
    DWORD nameSize = 0, infoSize = 0;
    BYTE* names = SpeedReg(PERF_REG_COUNTER_ENGLISH_NAMES, nameSize); BYTE* info = SpeedReg(PERF_REG_COUNTERSET_STRUCT, infoSize);
    DWORD perf = ~0u, perfBase = ~0u, freq = ~0u;
    if (names && info && nameSize >= sizeof(PERF_STRING_BUFFER_HEADER) && infoSize >= sizeof(PERF_COUNTERSET_REG_INFO)) {
        const PERF_STRING_BUFFER_HEADER* sh = (const PERF_STRING_BUFFER_HEADER*)names;
        const PERF_STRING_COUNTER_HEADER* sc = (const PERF_STRING_COUNTER_HEADER*)(sh + 1);
        const PERF_COUNTERSET_REG_INFO* ci = (const PERF_COUNTERSET_REG_INFO*)info;
        const PERF_COUNTER_REG_INFO* cr = (const PERF_COUNTER_REG_INFO*)(ci + 1);
        for (DWORD i = 0; i < ci->NumCounters && (BYTE*)(cr + i + 1) <= info + infoSize; i++)
            for (DWORD k = 0; k < sh->dwCounters && (BYTE*)(sc + k + 1) <= names + nameSize; k++) {
                if (sc[k].dwCounterId != cr[i].CounterId || !sc[k].dwOffset || sc[k].dwOffset >= nameSize) continue;
                const wchar_t* name = (const wchar_t*)(names + sc[k].dwOffset);            // i contatori si cercano per nome inglese, non per numero
                if (!lstrcmpW(name, L"% Processor Performance")) { perf = cr[i].CounterId; perfBase = cr[i].BaseCounterId; }
                else if (!lstrcmpW(name, L"Processor Frequency")) freq = cr[i].CounterId;
            }
    }
    if (names) HeapFree(GetProcessHeap(), 0, names); if (info) HeapFree(GetProcessHeap(), 0, info);
    if (perf == ~0u || perfBase == ~0u || freq == ~0u) return false;
    if (PerfOpenQueryHandle(nullptr, &g_speed.query) != ERROR_SUCCESS) { g_speed.query = nullptr; return false; }
    // nell'ordine in cui tornano i dati: il contatore, la sua base, la frequenza nominale
    if (SpeedAdd(g_speed.query, perf) && SpeedAdd(g_speed.query, perfBase) && SpeedAdd(g_speed.query, freq)) return true;
    PerfCloseQueryHandle(g_speed.query); g_speed.query = nullptr; return false;
}
// MHz dall'ultima lettura; 0 alla prima o se i contatori non rispondono
static float SpeedRead()
{
    if (!g_speed.tried) { g_speed.tried = true; if (!SpeedOpen()) LogLine("cpu speed: PerfLib counters not available, using the clock Windows reports"); }
    if (!g_speed.query) return 0;
    static BYTE data[512]; DWORD used = 0;
    if (PerfQueryCounterData(g_speed.query, (PERF_DATA_HEADER*)data, sizeof(data), &used) != ERROR_SUCCESS) return 0;
    const PERF_DATA_HEADER* h = (const PERF_DATA_HEADER*)data;
    if (h->dwNumCounters != 3) return 0;
    ULONGLONG v[3]; bool wide[3]; DWORD at = sizeof(PERF_DATA_HEADER);
    for (int i = 0; i < 3; i++) {
        if (at + sizeof(PERF_COUNTER_HEADER) + sizeof(PERF_COUNTER_DATA) + 4 > sizeof(data)) return 0;
        const PERF_COUNTER_HEADER* ch = (const PERF_COUNTER_HEADER*)(data + at);
        if (ch->dwStatus != ERROR_SUCCESS || ch->dwType != PERF_SINGLE_COUNTER) return 0;   // un valore solo, quello dell'istanza _Total
        const PERF_COUNTER_DATA* cd = (const PERF_COUNTER_DATA*)(ch + 1);
        wide[i] = cd->dwDataSize >= 8;
        v[i] = wide[i] ? *(const ULONGLONG*)(cd + 1) : *(const DWORD*)(cd + 1);
        at += ch->dwSize;
    }
    // un contatore a 32 bit che ha fatto il giro riparte da zero: la differenza si prende modulo 2^32
    ULONGLONG perf = wide[0] ? v[0] - g_speed.prevPerf : (DWORD)(v[0] - g_speed.prevPerf), base = wide[1] ? v[1] - g_speed.prevBase : (DWORD)(v[1] - g_speed.prevBase);
    bool primed = g_speed.primed; g_speed.prevPerf = v[0]; g_speed.prevBase = v[1]; g_speed.primed = true;
    if (!primed || !base || !v[2]) return 0;
    return (float)((double)v[2] * ((double)perf / (double)base) / 100);
}

static void ReadClock(SlowData& d)
{
    static int misses;
    float mhz = SpeedRead();
    if (mhz > 100 && mhz < 20000) { d.cpuGhz = mhz / 1000; d.clockKind = 2; misses = 0; return; }
    // la prima lettura dà solo il punto di partenza, e una lettura mancata ogni tanto non fa cambiare fonte
    if (g_speed.query && ++misses < 4) return;
    static PowerInfo info[MAXCORE];
    SYSTEM_INFO si; GetSystemInfo(&si); ULONG n = si.dwNumberOfProcessors; if (n > MAXCORE) n = MAXCORE;
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info, n * sizeof(PowerInfo)) != 0) return;
    ULONG best = 0; for (ULONG i = 0; i < n; i++) if (info[i].currentMhz > best) best = info[i].currentMhz;
    d.cpuGhz = best / 1000.f; d.clockKind = 1;
}

static void ReadDrives(SlowData& d)
{
    Drive out[MAXDRIVE]; int n = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && n < MAXDRIVE; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER avail, total, free;
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &free) || !total.QuadPart) continue;   // un lettore vuoto non è un disco
        out[n].name[0] = root[0]; out[n].name[1] = L':'; out[n].name[2] = 0;
        out[n].freeGb = avail.QuadPart / 1073741824.f; out[n].totalGb = total.QuadPart / 1073741824.f; n++;
    }
    CopyMemory(d.drives, out, sizeof(out)); d.ndrive = n;
}

// la scheda che porta fuori: accesa, con un gateway, non di loopback né un tunnel
static void ReadAdapter(SlowData& d)
{
    static BYTE buf[48 * 1024]; ULONG len = sizeof(buf);
    d.adapter[0] = 0; d.localIp[0] = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)buf, &len) != NO_ERROR) return;
    const IP_ADAPTER_ADDRESSES* best = nullptr;
    for (const IP_ADAPTER_ADDRESSES* a = (const IP_ADAPTER_ADDRESSES*)buf; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (!a->FirstGatewayAddress || !a->FirstUnicastAddress) continue;
        if (!best || a->Ipv4Metric < best->Ipv4Metric) best = a;
    }
    if (!best) return;
    lstrcpynW(d.adapter, best->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi" : best->IfType == IF_TYPE_ETHERNET_CSMACD ? L"Ethernet" : L"Network", 16);
    const sockaddr* sa = best->FirstUnicastAddress->Address.lpSockaddr;
    if (sa && sa->sa_family == AF_INET) {
        const BYTE* b = (const BYTE*)&((const sockaddr_in*)sa)->sin_addr;
        Sb ip; ip.i(b[0]).c(L'.').i(b[1]).c(L'.').i(b[2]).c(L'.').i(b[3]); lstrcpynW(d.localIp, ip.s, 48);
    }
}

static void ReadPing(SlowData& d)
{
    static HANDLE icmp = INVALID_HANDLE_VALUE;
    if (icmp == INVALID_HANDLE_VALUE) icmp = IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE) { d.pingMs = NONE; return; }
    char data[8] = { 'p', 'u', 'l', 's', 'e', 0, 0, 0 }; BYTE reply[sizeof(ICMP_ECHO_REPLY) + 8 + 16];
    DWORD n = IcmpSendEcho(icmp, 0x08080808 /* 8.8.8.8 */, data, sizeof(data), nullptr, reply, sizeof(reply), 1000);
    const ICMP_ECHO_REPLY* r = (const ICMP_ECHO_REPLY*)reply;
    d.pingMs = n && r->Status == IP_SUCCESS ? (float)r->RoundTripTime : NONE;
}

// l'indirizzo con cui il PC esce su Internet: lo dice un servizio pubblico, a cui non si manda niente
static void ReadPublicIp(SlowData& d)
{
    char text[64] = ""; DWORD got = 0;
    HINTERNET ses = WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) {
        WinHttpSetTimeouts(req, 5000, 5000, 5000, 5000);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr))
            WinHttpReadData(req, text, sizeof(text) - 1, &got);
    }
    if (req) WinHttpCloseHandle(req); if (con) WinHttpCloseHandle(con); if (ses) WinHttpCloseHandle(ses);
    text[got < sizeof(text) ? got : 0] = 0;
    // solo qualcosa che ha la forma di un indirizzo finisce sul pannello
    int n = 0; bool ok = got >= 7;
    for (; ok && text[n]; n++) { char c = text[n]; if (!((c >= '0' && c <= '9') || c == '.' || c == ':' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) ok = false; }
    if (ok && n <= 45) { for (int i = 0; i <= n; i++) d.publicIp[i] = (wchar_t)text[i]; }
}

// ---------------------------------------------------------------------------------------------------------------------------
static SlowData g_work;                  // quello che il thread sta riempiendo
static void Publish() { EnterCriticalSection(&g_lock); g_data = g_work; LeaveCriticalSection(&g_lock); }

// la scheda e l'indirizzo cambiano di rado: si rileggono quando Windows avvisa di un cambio, e comunque una volta al minuto
static volatile LONG g_adapterDirty = 1;
static void CALLBACK OnAddressChange(void*, MIB_IPINTERFACE_ROW*, MIB_NOTIFICATION_TYPE) { InterlockedExchange(&g_adapterDirty, 1); }

static DWORD WINAPI SlowThread(void*)
{
    bool noPublicIp = GetEnvironmentVariableW(L"PULSE_NO_PUBLIC_IP", nullptr, 0) != 0;
    HANDLE notify = nullptr; NotifyIpInterfaceChange(AF_UNSPEC, OnAddressChange, nullptr, FALSE, &notify);
    for (unsigned tick = 0;; tick++) {                                                   // un giro ogni secondo
        // la prima scansione dà solo il punto di partenza: la seconda arriva dopo un secondo, non dopo cinque
        if (tick % 5 == 0 || tick == 1) { ReadPrograms(g_work); Publish(); }
        if (tick % 2 == 0) {
            ReadGpu(g_work); ReadClock(g_work);
            if (tick % 60 == 0 || InterlockedExchange(&g_adapterDirty, 0)) ReadAdapter(g_work);
            Publish(); ReadPing(g_work); Publish();
        }
        if (tick % 30 == 0) { ReadDrives(g_work); Publish(); }
        // ogni dieci minuti; finché non c'è, un tentativo al minuto
        if (!noPublicIp && (tick % 600 == 3 || (!g_work.publicIp[0] && tick % 60 == 3))) { ReadPublicIp(g_work); Publish(); }
        if (WaitForSingleObject(g_stop, 1000) != WAIT_TIMEOUT) break;
    }
    if (notify) CancelMibChangeNotify2(notify);
    return 0;
}

void SlowStart()
{
    InitializeCriticalSection(&g_lock);
    g_work.pingMs = g_work.gpuTemp = g_work.gpuWatt = NONE; g_data = g_work;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, SlowThread, nullptr, 0, nullptr);
}
void SlowStop()
{
    if (!g_thread) return;
    SetEvent(g_stop); WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr;
}
void SlowOnce()
{
    static bool init; if (!init) { InitializeCriticalSection(&g_lock); g_work.pingMs = g_work.gpuTemp = g_work.gpuWatt = NONE; init = true; }
    ReadPrograms(g_work); ReadGpu(g_work); ReadClock(g_work); ReadAdapter(g_work); ReadPing(g_work); ReadDrives(g_work); Publish();
}
void SlowCopy(State& s)
{
    EnterCriticalSection(&g_lock);
    const SlowData& d = g_data;
    s.gpuOk = d.gpuOk; s.gpu = d.gpu; s.gpuTemp = d.gpuTemp; s.gpuWatt = d.gpuWatt; s.vramUsed = d.vramUsed; s.vramTotal = d.vramTotal;
    s.gpuClock = d.gpuClock; s.gpuMemClock = d.gpuMemClock; lstrcpynW(s.gpuName, d.gpuName, 64);
    s.cpuGhz = d.cpuGhz; s.clockKind = d.clockKind; s.pingMs = d.pingMs;
    lstrcpynW(s.adapter, d.adapter, 16); lstrcpynW(s.localIp, d.localIp, 48); lstrcpynW(s.publicIp, d.publicIp, 48);
    CopyMemory(s.drives, d.drives, sizeof(d.drives)); s.ndrive = d.ndrive;
    CopyMemory(s.topCpu, d.topCpu, sizeof(d.topCpu)); s.nTopCpu = d.nTopCpu; CopyMemory(s.topRam, d.topRam, sizeof(d.topRam)); s.nTopRam = d.nTopRam;
    LeaveCriticalSection(&g_lock);
}
