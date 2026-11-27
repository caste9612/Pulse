// Pulse 2: parti comuni. Niente librerie esterne, niente eccezioni, niente libreria standard del C++: solo Windows.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <math.h>

enum { HIST = 60, MAXCORE = 256, NTOP = 6, MAXDRIVE = 6, NFILE = 5, PROGNAME = 40 };
// un valore che non c'è (temperatura senza sensore, consumo che nessuno legge): mai un numero inventato al suo posto
const float NONE = -1;
inline bool Has(float v) { return v >= 0; }

inline float Clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float MaxOf(const float* v, int n = HIST) { float m = v[0]; for (int i = 1; i < n; i++) if (v[i] > m) m = v[i]; return m; }
// posizione su scala logaritmica di una velocità in byte al secondo: 1 KB/s = 0, 1 GB/s = 1
inline float Log6(float bytes) { float kb = bytes / 1024; return Clampf(log10f(kb < 1 ? 1 : kb) / 6, 0, 1); }

// ---- i dati che il pannello disegna ----
// un programma: tutti i processi con lo stesso nome, sommati
struct Program {
    wchar_t name[PROGNAME];
    int count;                          // quanti processi
    float cpu;                          // percento di tutti i processori
    float ramMb;                        // somma dei working set
    DWORD pid;                          // il processo più pesante: quello di cui si aprono cartella e proprietà
    int slot;                           // colore nelle barre dei core: 0..2, -1 non seguito, -2 campionamento non disponibile
};
struct Drive { wchar_t name[4]; float freeGb, totalGb; };

struct State {
    // CPU
    int ncore; float cores[MAXCORE]; unsigned char phys[MAXCORE];   // carico 0..1 di ogni core logico e il core fisico di cui fa parte
    float cpu, coresInUse;              // percento e somma dei carichi
    float cpuGhz;                       // 0 = non si sa
    float cpuTemp, cpuWatt;             // NONE finché non c'è il processo dei sensori
    // RAM, in GB
    float ramPct, ramUsed, ramTotal, ramAvail;
    bool ramParts; float ramInUse, ramModified, ramStandby, ramFree;
    // GPU
    bool gpuOk; float gpu, gpuTemp, gpuWatt, vramUsed, vramTotal, gpuClock, gpuMemClock; wchar_t gpuName[64];
    // rete e disco, in byte al secondo
    float down, up, dr, dw;
    wchar_t adapter[16], localIp[48], publicIp[48]; float pingMs;
    // dischi
    Drive drives[MAXDRIVE]; int ndrive;
    // l'ultimo minuto, un valore al secondo; le temperature valgono NONE dove non c'erano
    float hCpu[HIST], hCt[HIST], hRam[HIST], hGpu[HIST], hGt[HIST], hDown[HIST], hUp[HIST], hDr[HIST], hDw[HIST];
    // programmi
    Program topCpu[NTOP], topRam[NTOP]; int nTopCpu, nTopRam;
    // sistema
    ULONGLONG uptimeSec;
};

// letture veloci, a ogni secondo, sul thread della finestra (metrics.cpp)
void MetricsInit(State& s);
void MetricsTick(State& s);
// letture lente, su un thread a parte: GPU e rete ogni 2 s, programmi ogni 5 s, dischi ogni 30 s (slow.cpp)
void SlowStart();
void SlowStop();
void SlowCopy(State& s);                // porta nel pannello le ultime letture lente
void SlowOnce();                        // un giro completo, subito (per --shot e --dump)
void LogLine(const char* s);            // %TEMP%\pulse2.log

// ---- testi: un piccolo costruttore di stringhe, senza printf ----
struct Sb {
    wchar_t s[128]; int n;
    Sb() : n(0) { s[0] = 0; }
    Sb& clear() { n = 0; s[0] = 0; return *this; }
    Sb& c(wchar_t ch) { if (n < 127) { s[n++] = ch; s[n] = 0; } return *this; }
    Sb& t(const wchar_t* p) { while (*p) c(*p++); return *this; }
    Sb& i(long long v) {
        if (v < 0) { c(L'-'); v = -v; }
        wchar_t b[24]; int k = 0; do { b[k++] = (wchar_t)(L'0' + v % 10); v /= 10; } while (v);
        while (k) c(b[--k]); return *this;
    }
    Sb& r(float v) { return i((long long)(v + .5f)); }                                    // arrotondato all'intero
    Sb& f1(float v) { long long x = (long long)(v * 10 + .5f); i(x / 10); c(L'.'); c((wchar_t)(L'0' + x % 10)); return *this; }
    Sb& f2(float v) { long long x = (long long)(v * 100 + .5f); i(x / 100); c(L'.'); c((wchar_t)(L'0' + x / 10 % 10)); c((wchar_t)(L'0' + x % 10)); return *this; }
    Sb& sep() { if (n) t(L" \x00B7 "); return *this; }                                    // il punto a mezza altezza tra due parti di una riga
    // gli stessi formati di Pulse 1.x
    Sb& rate(float b) {
        if (b >= 1073741824.f) { f2(b / 1073741824.f); t(L" GB/s"); } else if (b >= 1048576.f) { f1(b / 1048576.f); t(L" MB/s"); }
        else if (b >= 1024) { r(b / 1024); t(L" KB/s"); } else { r(b); t(L" B/s"); }
        return *this;
    }
    Sb& ram(float mb) { if (mb >= 1024) { f2(mb / 1024); t(L" GB"); } else { r(mb); t(L" MB"); } return *this; }
    Sb& gb(float g) { if (g >= 1000) { f1(g / 1024); t(L" TB"); } else if (g >= 100) { r(g); t(L" GB"); } else { f1(g); t(L" GB"); } return *this; }
};

// scala tonda per i grafici di rete e disco, a passi 1-2-5 da 100 KB/s a 10 GB/s: la più piccola che contiene il massimo del minuto
struct Nice { float max; wchar_t label[16]; };
Nice NiceScale(float maxBytes);

// ---- colori (0xRRGGBB), quelli del tema scuro di Reticle usato da Pulse ----
enum : unsigned {
    C_BG = 0x121518, C_SURF2 = 0x191d21, C_LINE = 0x2c3338, C_LINESOFT = 0x1d2226, C_DOTS = 0x2b3237,
    C_TEXT = 0xe9edee, C_TEXT2 = 0xaab3b8, C_MUTED = 0x6c7780, C_ACCENT = 0x3ddc84,
    C_CPU = 0x4cc2ff, C_RAM = 0xb987f7, C_GPU = 0x3ddc84, C_NET = 0xf5c84b, C_DSK = 0xff5a52, C_TEMP = 0xff8a1f,
    C_P0 = 0xff6eb4, C_P1 = 0xb8e55c, C_P2 = 0x8c9eff
};

// ---- caratteri: i cinque file di Reticle che servono, dentro l'exe come risorse 101..105 ----
enum FontFile { FF_BARLOW, FF_COND_SEMI, FF_COND_BOLD, FF_CHAKRA, FF_MONO };
enum FontId { F_LABEL, F_NAME, F_BIG, F_C15, F_TEXT, F_L2, F_SMALL, F_MONO, F_MONO9, F_COUNT };
struct FontSpec { FontFile file; float px; float track; float cap; };               // track in em, cap = altezza delle maiuscole in em
static const FontSpec kFonts[F_COUNT] = {
    { FF_CHAKRA,    10.5f, .16f, .70f },   // F_LABEL   etichette CPU, RAM...
    { FF_CHAKRA,    11.5f, .20f, .70f },   // F_NAME    PULSE
    { FF_COND_SEMI, 22.0f, 0,    .70f },   // F_BIG     il valore grande
    { FF_COND_SEMI, 15.0f, 0,    .70f },   // F_C15     il valore secondario e le velocità su due righe
    { FF_BARLOW,    12.5f, 0,    .70f },   // F_TEXT
    { FF_BARLOW,    11.5f, 0,    .70f },   // F_L2      riga di dettaglio
    { FF_BARLOW,    11.0f, 0,    .70f },   // F_SMALL
    { FF_MONO,      11.5f, 0,    .73f },   // F_MONO    valori negli elenchi
    { FF_MONO,       9.5f, 0,    .73f },   // F_MONO9   righello e scale
};
struct FontData { const void* p; DWORD n; };
extern FontData g_fontData[NFILE];

// ---- icone della testata: gli stessi tracciati di Pulse 1.x, in una casella 16 x 16 ----
static const float kLogoPts[18] = { 1.6f,8, 5.1f,8, 6.1f,9.3f, 7,8, 8,3.5f, 8.8f,12.5f, 9.9f,8, 10.9f,8, 14.4f,8 };
struct Seg { int icon; int n; float p[12]; };
static const Seg kIconSegs[] = {
    { 0, 2, { 3,2.5f, 13,2.5f } }, { 0, 2, { 3,13.5f, 13,13.5f } }, { 0, 2, { 8,5, 8,11 } }, { 0, 3, { 6,7, 8,5, 10,7 } }, { 0, 3, { 6,9, 8,11, 10,9 } },
    { 1, 3, { 2.5f,6.5f, 2.5f,2.5f, 6.5f,2.5f } }, { 1, 3, { 9.5f,2.5f, 13.5f,2.5f, 13.5f,6.5f } }, { 1, 3, { 13.5f,9.5f, 13.5f,13.5f, 9.5f,13.5f } }, { 1, 3, { 6.5f,13.5f, 2.5f,13.5f, 2.5f,9.5f } },
    { 2, 2, { 5.5f,1.5f, 10.5f,1.5f } }, { 2, 6, { 6.5f,1.5f, 6.5f,6.5f, 4.5f,9.5f, 11.5f,9.5f, 9.5f,6.5f, 9.5f,1.5f } }, { 2, 2, { 8,9.5f, 8,14.5f } },
    { 3, 2, { 3,8, 13,8 } },
};
enum { NICONSEG = sizeof(kIconSegs) / sizeof(kIconSegs[0]) };

// ---- il disegno (draw.cpp): Direct2D via software, fondo opaco ----
enum { PANEL_W = 514 };                                                              // larghezza predefinita, in unità (96 per pollice)
float PanelHeight(const State& s);                                                   // quanto è alto il contenuto, in unità
bool DrawCreate(HWND hwnd);
void DrawResize(HWND hwnd);
void DrawPaint(HWND hwnd, const State& s);
bool DrawToDc(HDC memdc, int w, int h, const State& s);                              // una resa fuori schermo, senza finestra
