// Pulse 2: parti comuni. Niente librerie esterne, niente eccezioni, niente libreria standard del C++: solo Windows.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <math.h>

enum { HIST = 60, MAXCORE = 256, NTOP = 6, MAXDRIVE = 6, NFILE = 5, PROGNAME = 40, MAXPID = 64 };
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
    DWORD pids[MAXPID]; int npid;       // i suoi processi (i primi 64), per il campionamento dei core; solo in Top CPU
    int slot;                           // colore nelle barre dei core: 0..2, -1 non seguito, -2 campionamento non disponibile
};
struct Drive { wchar_t name[4]; float freeGb, totalGb; };

struct State {
    // CPU
    int ncore; float cores[MAXCORE]; unsigned char phys[MAXCORE];   // carico 0..1 di ogni core logico e il core fisico di cui fa parte
    float cpu, coresInUse;              // percento e somma dei carichi
    float cpuGhz;                       // 0 = non si sa
    int clockKind;                      // 1 = quella che dice Windows (su molti processori la nominale), 2 = la media dei core al lavoro
    float cpuTemp, cpuWatt;             // NONE senza il processo dei sensori
    int sensorFlags;                    // 0 = il processo dei sensori non c'è; altrimenti i suoi SF_ (shared.h)
    float share[MAXCORE][3];            // per core logico, la parte del carico di ciascun programma seguito (0..1)
    // RAM, in GB
    float ramPct, ramUsed, ramTotal, ramAvail;
    bool ramParts; float ramInUse, ramModified, ramStandby, ramFree;
    float commit, commitLimit, pageUsed, pageTotal;                   // memoria impegnata e file di paging, per il tooltip
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
void SlowGpuOff();                      // il driver video è cambiato: la libreria NVIDIA non si chiama più fino al riavvio
// il dialogo col processo dei sensori, l'unico con i diritti di amministratore (sensors.cpp)
void SensorsInit();
void SensorsExchange(State& s);         // una volta al secondo, dopo le letture
void LogLine(const char* s);            // %TEMP%\pulse2.log

// ---- testi: un piccolo costruttore di stringhe, senza printf ----
template <int N> struct SbT {
    wchar_t s[N]; int n;
    SbT() : n(0) { s[0] = 0; }
    SbT& clear() { n = 0; s[0] = 0; return *this; }
    SbT& c(wchar_t ch) { if (n < N - 1) { s[n++] = ch; s[n] = 0; } return *this; }
    SbT& t(const wchar_t* p) { while (*p) c(*p++); return *this; }
    SbT& i(long long v) {
        if (v < 0) { c(L'-'); v = -v; }
        wchar_t b[24]; int k = 0; do { b[k++] = (wchar_t)(L'0' + v % 10); v /= 10; } while (v);
        while (k) c(b[--k]); return *this;
    }
    SbT& r(float v) { return i((long long)(v + .5f)); }                                    // arrotondato all'intero
    SbT& f1(float v) { long long x = (long long)(v * 10 + .5f); i(x / 10); c(L'.'); c((wchar_t)(L'0' + x % 10)); return *this; }
    SbT& f2(float v) { long long x = (long long)(v * 100 + .5f); i(x / 100); c(L'.'); c((wchar_t)(L'0' + x / 10 % 10)); c((wchar_t)(L'0' + x % 10)); return *this; }
    SbT& sep() { if (n) t(L" \x00B7 "); return *this; }                                    // il punto a mezza altezza tra due parti di una riga
    // gli stessi formati di Pulse 1.x
    SbT& rate(float b) {
        if (b >= 1073741824.f) { f2(b / 1073741824.f); t(L" GB/s"); } else if (b >= 1048576.f) { f1(b / 1048576.f); t(L" MB/s"); }
        else if (b >= 1024) { r(b / 1024); t(L" KB/s"); } else { r(b); t(L" B/s"); }
        return *this;
    }
    SbT& ram(float mb) { if (mb >= 1024) { f2(mb / 1024); t(L" GB"); } else { r(mb); t(L" MB"); } return *this; }
    SbT& gb(float g) { if (g >= 1000) { f1(g / 1024); t(L" TB"); } else if (g >= 100) { r(g); t(L" GB"); } else { f1(g); t(L" GB"); } return *this; }
};
typedef SbT<128> Sb;                    // una riga del pannello
typedef SbT<1100> Lb;                   // un tooltip di più righe

// scala tonda per i grafici di rete e disco, a passi 1-2-5 da 100 KB/s a 10 GB/s: la più piccola che contiene il massimo del minuto
struct Nice { float max; wchar_t label[16]; };
Nice NiceScale(float maxBytes);

// ---- colori (0xRRGGBB): i due temi di Reticle usati da Pulse, scuro e chiaro ----
struct Theme {
    unsigned bg, surf2, line, lineSoft, dots, text, text2, muted, accent, cpu, ram, gpu, net, dsk, temp, p0, p1, p2;
    float grid;                                                                      // forza delle righe del tempo nei grafici
    bool light;
};
extern Theme g_dark, g_light;                                                        // non costanti: l'accento si sceglie
extern const Theme* g_theme;                                                         // quello in uso
#define C_BG (g_theme->bg)
#define C_SURF2 (g_theme->surf2)
#define C_LINE (g_theme->line)
#define C_LINESOFT (g_theme->lineSoft)
#define C_DOTS (g_theme->dots)
#define C_TEXT (g_theme->text)
#define C_TEXT2 (g_theme->text2)
#define C_MUTED (g_theme->muted)
#define C_ACCENT (g_theme->accent)
#define C_CPU (g_theme->cpu)
#define C_RAM (g_theme->ram)
#define C_GPU (g_theme->gpu)
#define C_NET (g_theme->net)
#define C_DSK (g_theme->dsk)
#define C_TEMP (g_theme->temp)
#define C_P0 (g_theme->p0)
#define C_P1 (g_theme->p1)
#define C_P2 (g_theme->p2)

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
enum { PANEL_W = 514, HEADER_H = 32, BTN_W = 28, NBTN = 4 };                         // misure in unità (96 per pollice)
enum { BTN_FIT, BTN_CORNER, BTN_PIN, BTN_HIDE };                                     // i quattro bottoni della testata, da sinistra
inline float BtnX(float panelW, int k) { return panelW - 4 - BTN_W * NBTN + k * BTN_W; }
// quello che il mouse sta facendo: il bottone della testata sotto il mouse (-1 nessuno), la riga di Top CPU o Top RAM sotto il mouse
// (il suo RG_, 0 nessuna), la scelta dell'angolo aperta e la sua freccia sotto il mouse (-1 nessuna), il secondo dei grafici sotto
// il mouse (la sua posizione nel minuto + 1, 0 nessuno)
struct Ui { int hot; bool pinned; int hotRegion; bool picker; int hotArrow; int cross; };
// la scelta dell'angolo: una tessera di due caselle per due sotto il bottone, 0 alto sx, 1 alto dx, 2 basso sx, 3 basso dx
enum { PICK_CELL = 28, PICK_PAD = 4 };
inline float PickerX(float panelW)
{
    float side = 2 * (PICK_PAD + PICK_CELL), x = BtnX(panelW, BTN_CORNER) + BTN_W / 2 - side / 2;
    return x + side > panelW - 4 ? panelW - 4 - side : x;
}
inline void PickerCell(float panelW, int k, float& x, float& y) { x = PickerX(panelW) + PICK_PAD + (float)((k & 1) * PICK_CELL); y = (float)(HEADER_H + PICK_PAD + (k >> 1) * PICK_CELL); }
float PanelHeight(const State& s);                                                   // quanto è alto il contenuto
// le zone del pannello che hanno un tooltip o un menu, nelle stesse unità del disegno
enum { RG_POWER = 1, RG_CPU_TEMP, RG_CORES, RG_RAM, RG_GPU, RG_BTN = 10, RG_TOPCPU = 100, RG_TOPRAM = 200 };
struct Region { int id; float x, y, w, h; };
int PanelRegions(const State& s, float W, Region* out, int cap);
float PanelFitWidth(const State& s);                                                 // la larghezza più stretta in cui niente è tagliato
int GraphSecondAt(const State& s, float W, float x, float y);                        // il secondo (0..HIST-1) del grafico sotto un punto, -1 se lì non c'è un grafico
bool DrawCreate(HWND hwnd);
void DrawResize(HWND hwnd);
void DrawThemeChanged();                                                             // il tema è cambiato: si rifanno letto puntinato e sfumature
void DrawSetAccent(unsigned dark, unsigned lightBase);                               // l'accento nel tema scuro, e il colore da cui si ricava quello del chiaro
void DrawSetDensity(int density);                                                    // 0 compatta, 1 comoda: cambia l'altezza del pannello
void DrawPaint(HWND hwnd, const State& s, const Ui& ui);
bool DrawToDc(HDC memdc, int w, int h, float dpi, const State& s, const Ui& ui);     // una resa fuori schermo, senza finestra; w e h in pixel

// ---- tooltip e menu: due finestrelle disegnate da Pulse con i caratteri e il bordo del pannello (popups.cpp; il disegno è in draw.cpp) ----
enum { WM_TRAY = WM_APP + 1, WM_MENU_CLOSED, WM_GPU_CHANGED };                       // i messaggi privati della finestra del pannello
// i comandi che arrivano al pannello come WM_COMMAND: dal menu del tray, da quello di un programma, dalla finestra Aspetto
enum { ID_SHOW = 1, ID_DARK, ID_LIGHT, ID_FOLLOW, ID_TOPMOST, ID_AUTOSTART, ID_EXIT, ID_APPEARANCE,
       ID_CORNER = 10,                                                               // + 0..3: alto sx, alto dx, basso sx, basso dx
       ID_P_OPEN = 20, ID_P_PROPS, ID_P_END, ID_P_SEARCH,                            // il menu di un programma di Top CPU o Top RAM
       ID_ACCENT = 30, ID_OPACITY = 40, ID_DENSITY = 50 };                           // + la scelta, dalla finestra Aspetto
enum { MI_ITEM, MI_SEP, MI_HEAD };                                                   // una voce, una riga di separazione, un titolo che non si sceglie
struct MenuItem { int kind, id; bool checked; wchar_t text[40]; };
struct Menu {
    MenuItem it[16]; int n, hot;                                                     // hot: la voce sotto il mouse o raggiunta coi tasti, -1 nessuna
    Menu() : n(0), hot(-1) {}
    Menu& add(int kind, int id = 0, const wchar_t* text = L"", bool checked = false)
    {
        if (n < 16) { MenuItem& m = it[n++]; m.kind = kind; m.id = id; m.checked = checked; lstrcpynW(m.text, text, 40); }
        return *this;
    }
};
void TipSize(const wchar_t* text, float& w, float& h);                               // in unità; 0 se non c'è niente da scrivere
void TipPaint(HDC dc, int wpx, int hpx, float dpi, const wchar_t* text);
void MenuSize(const Menu& m, float& w, float& h);
int MenuItemAt(const Menu& m, float y);                                              // la voce a quell'altezza (in unità), -1 se lì non c'è una voce che si sceglie
void MenuPaint(HDC dc, int wpx, int hpx, float dpi, const Menu& m);
void PopupsInit(HINSTANCE inst, HWND panel);
void TipShow(const wchar_t* text, POINT at);                                         // sotto quel punto dello schermo; se è già aperto resta dov'è e cambia solo il testo
void TipHide();
void MenuOpen(const Menu& m, POINT at);                                              // la voce scelta arriva al pannello come WM_COMMAND; alla chiusura, WM_MENU_CLOSED
bool MenuIsOpen();
void PopupsRedraw();                                                                 // il tema è cambiato

// ---- la finestra Aspetto: accento, tema, trasparenza e densità. Ogni scelta vale subito e resta salvata ----
enum { AP_ACCENT, AP_THEME, AP_OPACITY, AP_DENSITY, AP_GROUPS };
enum { NACCENT = 5 };                                                                // verde, ambra, ciano, lime (quelli di Reticle), e il colore di Windows
struct Appearance { int value[AP_GROUPS]; int hot; unsigned swatch[NACCENT]; };      // hot: gruppo * 16 + scelta sotto il mouse, -1 nessuna
void AppearanceGet(Appearance& a);                                                   // le scelte in uso (main.cpp)
void AppearanceSize(float& w, float& h);
int AppearanceAt(float x, float y);                                                  // gruppo * 16 + scelta a quel punto, -1 se non c'è
void AppearancePaint(HDC dc, int wpx, int hpx, float dpi, const Appearance& a);
void AppearanceOpen();                                                               // la scelta arriva al pannello come WM_COMMAND
