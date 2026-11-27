// Il lato del pannello nel dialogo col processo dei sensori. Il pannello dice quali programmi di Top CPU vuole vedere a colori
// nelle barre dei core; il processo dei sensori, se c'è, risponde con temperatura, consumo e frequenza della CPU e con la parte di
// ogni core usata da quei programmi. Senza di lui il pannello fa tutto il resto come prima: la temperatura dice n/a e le barre sono
// di un colore solo.
#include "app.h"
#include "shared.h"

static Shared* g_shared;
static wchar_t g_slot[NSLOT][PROGNAME];  // i programmi che hanno un colore, ognuno finché non scende sotto un quarto di core

void SensorsInit()
{
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, SHARED_SIZE, L"Local\\Pulse2.Shared");
    if (!map) { LogLine("sensors: shared memory not created"); return; }
    g_shared = (Shared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, SHARED_SIZE);   // la mappa resta aperta finché vive il pannello
    if (!g_shared) return;
    ZeroMemory(&g_shared->panel, sizeof(g_shared->panel));
    g_shared->magic = SHARED_MAGIC; g_shared->version = SHARED_VERSION;
}

static int SlotOf(const wchar_t* name) { for (int k = 0; k < NSLOT; k++) if (g_slot[k][0] && !lstrcmpiW(g_slot[k], name)) return k; return -1; }

// una volta al secondo, dopo le letture
void SensorsExchange(State& s)
{
    s.sensorFlags = 0; s.cpuTemp = s.cpuWatt = NONE;
    for (int i = 0; i < s.ncore; i++) s.share[i][0] = s.share[i][1] = s.share[i][2] = 0;
    for (int i = 0; i < s.nTopCpu; i++) s.topCpu[i].slot = -2;
    if (!g_shared) return;

    // quello che i sensori hanno scritto: vale se è fresco e intero
    SharedSensors in; bool alive = false;
    for (int attempt = 0; attempt < 3 && !alive; attempt++) {
        LONG seq = g_shared->sensors.seq; if (seq & 1) continue;
        CopyMemory(&in, &g_shared->sensors, sizeof(in));
        alive = g_shared->sensors.seq == seq && in.pid && GetTickCount64() - in.tick < 4000;
    }
    bool sampling = alive && (in.flags & SF_ELEVATED || in.flags & SF_FAKE) && !(in.flags & SF_NO_SAMPLING);
    if (alive) {
        s.sensorFlags = in.flags | SF_ALIVE;
        if (in.cpuTemp > 5) s.cpuTemp = in.cpuTemp;
        if (in.cpuWatt > 0) s.cpuWatt = in.cpuWatt;
        if (in.cpuMhz > 100) { s.cpuGhz = in.cpuMhz / 1000; s.clockKind = 3; }
        int n = in.shareCores < s.ncore ? in.shareCores : s.ncore;
        if (in.flags & SF_SAMPLING) for (int i = 0; i < n; i++) for (int k = 0; k < NSLOT; k++) s.share[i][k] = in.share[i][k] / 255.f;
    }

    // i programmi da colorare: entra chi usa almeno mezzo core, esce chi scende sotto un quarto (due soglie, così chi sta a cavallo
    // non lampeggia); chi esce libera il suo colore, chi entra ne prende uno libero. Fino a tre
    if (!sampling) ZeroMemory(g_slot, sizeof(g_slot));
    else {
        const Program* leader[NSLOT]; int nl = 0;
        for (int i = 0; i < s.nTopCpu && nl < NSLOT; i++) {
            float cores = s.topCpu[i].cpu * s.ncore / 100;
            if (cores >= (SlotOf(s.topCpu[i].name) >= 0 ? .25f : .5f)) leader[nl++] = &s.topCpu[i];
        }
        for (int k = 0; k < NSLOT; k++) {
            bool keep = false; for (int i = 0; i < nl; i++) if (g_slot[k][0] && !lstrcmpiW(g_slot[k], leader[i]->name)) keep = true;
            if (!keep) g_slot[k][0] = 0;
        }
        for (int i = 0; i < nl; i++) if (SlotOf(leader[i]->name) < 0) for (int k = 0; k < NSLOT; k++) if (!g_slot[k][0]) { lstrcpynW(g_slot[k], leader[i]->name, PROGNAME); break; }
        for (int i = 0; i < s.nTopCpu; i++) s.topCpu[i].slot = SlotOf(s.topCpu[i].name);
    }

    // quello che il pannello chiede
    SharedPanel& out = g_shared->panel;
    InterlockedIncrement(&out.seq);
    out.pid = GetCurrentProcessId(); out.tick = GetTickCount64(); out.ncore = s.ncore;
    for (int k = 0; k < NSLOT; k++) {
        lstrcpynW(out.slotName[k], g_slot[k], PROGNAME); out.slotCount[k] = 0;
        for (int i = 0; i < s.nTopCpu; i++) if (g_slot[k][0] && !lstrcmpiW(g_slot[k], s.topCpu[i].name)) {
            out.slotCount[k] = s.topCpu[i].npid; CopyMemory(out.slotPid[k], s.topCpu[i].pids, s.topCpu[i].npid * sizeof(DWORD));
        }
    }
    InterlockedIncrement(&out.seq);
}
