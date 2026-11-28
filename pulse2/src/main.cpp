// Pulse 2: la finestra e quello che le sta intorno. Un pannello senza cornice che si trascina dalla testata e si allarga dai lati
// (l'altezza è sempre quella del contenuto), quattro bottoni in testata (adatta, angolo, in primo piano, nascondi), un'icona nel
// tray col suo menu, le impostazioni in %APPDATA%\Pulse2\settings.txt, un'istanza sola.
//
//   --pos <x> <y>       posizione della finestra            --width <n>      larghezza in unità (predefinita 514)
//   --theme dark|light  tema, al posto di quello salvato    --seconds <n>    si chiude da solo dopo n secondi
//   --noactivate        non prende il fuoco quando si apre  --nosave         prova: non legge né scrive impostazioni, niente istanza unica
//   --shot <file.bmp>   legge i dati per un secondo, disegna un fotogramma fuori schermo, lo salva ed esce (nessuna finestra);
//                       con --demo-ui disegna anche una riga sotto il mouse e la scelta dell'angolo aperta, con --dpi <n> a
//                       un'altra scala (144 = 150 %), con --demo-data grafici ed elenchi hanno i dati finti di --bench
//   --dump <file.txt>   legge i dati per un secondo e li scrive come testo, per confrontarli con i contatori di Windows
//   --bench <n>         disegna n fotogrammi di fila con dati finti sempre uguali e scrive nel log quanta CPU è costato ognuno
#include "app.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <wtsapi32.h>
#include "shared.h"

FontData g_fontData[NFILE];
static State g_state;
static Ui g_ui = { -1, true, 0, false, -1 };
static HWND g_hwnd;
static HINSTANCE g_inst;
static int g_seconds; static unsigned g_ticks;
static bool g_nosave, g_noact, g_demoUi, g_demoData;
static int g_shotDpi = 96;
static bool g_locked, g_displayOff;                                                  // a schermo bloccato o spento non si disegna
static const GUID kDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };   // GUID_CONSOLE_DISPLAY_STATE
static int g_pressed = -1, g_pressedArrow = -1; static bool g_tracking;
static float g_minW = 420; static int g_minDrives = -1;
static UINT g_msgTaskbar, g_msgShow;
enum { WM_TRAY = WM_APP + 1 };
enum { ID_SHOW = 1, ID_DARK, ID_LIGHT, ID_FOLLOW, ID_TOPMOST, ID_AUTOSTART, ID_EXIT, ID_CORNER = 10,   // ID_CORNER + 0..3: alto sx, alto dx, basso sx, basso dx
       ID_P_OPEN = 20, ID_P_PROPS, ID_P_END, ID_P_SEARCH };                           // il menu di un programma di Top CPU o Top RAM
static HWND g_tip;                                                                    // il controllo dei tooltip
static Region g_rg[48]; static int g_nrg;                                             // le zone del pannello, rifatte quando cambia la misura
static wchar_t g_tipText[1100];
static Program g_menuProg;                                                            // il programma su cui è aperto il menu

// ---- piccoli attrezzi ----
static const wchar_t* Find(const wchar_t* s, const wchar_t* key)
{
    for (; *s; s++) { int i = 0; while (key[i] && s[i] == key[i]) i++; if (!key[i]) return s + i; }
    return nullptr;
}
static const wchar_t* SkipSp(const wchar_t* p) { while (*p == L' ') p++; return p; }
static int ReadInt(const wchar_t*& p)
{
    p = SkipSp(p); bool neg = *p == L'-'; if (neg) p++;
    int v = 0; while (*p >= L'0' && *p <= L'9') v = v * 10 + (*p++ - L'0');
    return neg ? -v : v;
}
static void ReadPath(const wchar_t* arg, wchar_t* path)
{
    int n = 0; arg = SkipSp(arg);
    bool q = *arg == L'"'; if (q) arg++;
    while (*arg && n < MAX_PATH - 1 && (q ? *arg != L'"' : *arg != L' ')) path[n++] = *arg++;
    path[n] = 0;
}

void LogLine(const char* s)
{
    wchar_t file[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, file); if (!n || n > MAX_PATH - 16) return;
    lstrcatW(file, L"pulse2.log");
    HANDLE h = CreateFileW(file, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t); char stamp[32]; wsprintfA(stamp, "%02d/%02d %02d:%02d:%02d ", t.wDay, t.wMonth, t.wHour, t.wMinute, t.wSecond);
    DWORD w; WriteFile(h, stamp, lstrlenA(stamp), &w, nullptr); WriteFile(h, s, lstrlenA(s), &w, nullptr); WriteFile(h, "\r\n", 2, &w, nullptr); CloseHandle(h);
}
static int MsSinceProcessStart()
{
    FILETIME c, e, k, u, now; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); GetSystemTimePreciseAsFileTime(&now);
    ULONGLONG a = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime, b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (int)((b - a) / 10000);
}
static void LoadFonts()
{
    for (int i = 0; i < NFILE; i++) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101 + i), RT_RCDATA);
        if (!r) continue;
        g_fontData[i].p = LockResource(LoadResource(nullptr, r)); g_fontData[i].n = SizeofResource(nullptr, r);
    }
}
static float Scale(HWND hwnd) { return GetDpiForWindow(hwnd) / 96.f; }
static int Px(HWND hwnd, float units) { return (int)(units * Scale(hwnd) + .5f); }

// ---- impostazioni: poche righe chiave=valore ----
struct Settings { bool have; int left, top, width; bool visible, topmost; int theme; };   // tema: 0 scuro, 1 chiaro, 2 come Windows
static Settings g_set = { false, 0, 0, PANEL_W, true, true, 0 };

static bool SettingsPath(wchar_t* path, bool create)
{
    DWORD n = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH - 32); if (!n || n > MAX_PATH - 32) return false;
    lstrcatW(path, L"\\Pulse2"); if (create) CreateDirectoryW(path, nullptr);
    lstrcatW(path, L"\\settings.txt"); return true;
}
static void SettingsLoad()
{
    wchar_t path[MAX_PATH]; char buf[512]; DWORD got = 0;
    if (g_nosave || !SettingsPath(path, false)) return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr); CloseHandle(f); buf[got] = 0;
    for (const char* p = buf; *p;) {
        char key[16]; int k = 0; while (*p && *p != '=' && *p != '\n' && k < 15) key[k++] = *p++; key[k] = 0;
        int v = 0; bool neg = false;
        if (*p == '=') { p++; if (*p == '-') { neg = true; p++; } while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'); if (neg) v = -v; }
        while (*p && *p != '\n') p++; if (*p) p++;
        if (!lstrcmpA(key, "left")) { g_set.left = v; g_set.have = true; } else if (!lstrcmpA(key, "top")) g_set.top = v;
        else if (!lstrcmpA(key, "width")) g_set.width = v; else if (!lstrcmpA(key, "visible")) g_set.visible = v != 0;
        else if (!lstrcmpA(key, "topmost")) g_set.topmost = v != 0; else if (!lstrcmpA(key, "theme")) g_set.theme = v;
    }
    if (g_set.width < 300 || g_set.width > 1600) g_set.width = PANEL_W;
    if (g_set.theme < 0 || g_set.theme > 2) g_set.theme = 0;
}
static void SettingsSave()
{
    wchar_t path[MAX_PATH]; char buf[160];
    if (g_nosave || !g_hwnd || !SettingsPath(path, true)) return;
    RECT rc; GetWindowRect(g_hwnd, &rc); g_set.left = rc.left; g_set.top = rc.top;
    int n = wsprintfA(buf, "left=%d\ntop=%d\nwidth=%d\nvisible=%d\ntopmost=%d\ntheme=%d\n", g_set.left, g_set.top, g_set.width, g_set.visible, g_set.topmost, g_set.theme);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, buf, n, &w, nullptr); CloseHandle(f);
}

// ---- avvio con Windows: il valore Pulse2 sotto HKCU\...\Run ----
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool Autostart()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Pulse2", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void SetAutostart(bool on)
{
    HKEY k; if (g_nosave || RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        cmd[0] = L'"'; lstrcpyW(cmd + 1, exe); lstrcatW(cmd, L"\"");
        RegSetValueExW(k, L"Pulse2", 0, REG_SZ, (const BYTE*)cmd, (lstrlenW(cmd) + 1) * 2);
    } else RegDeleteValueW(k, L"Pulse2");
    RegCloseKey(k);
}

// ---- tema ----
static bool WindowsIsLight()
{
    DWORD v = 0, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS && v != 0;
}
// I menu e i tooltip sono quelli di Windows: almeno nel tono del pannello, scuri col tema scuro. Per i menu Windows non ha una
// chiamata documentata: sono le due funzioni di uxtheme che usa Esplora risorse (numeri 135 e 136, da Windows 10 1903); se mancano
// i menu restano chiari, e basta
static void ControlsTheme(bool light)
{
    static bool tried; static int (WINAPI* setMode)(int); static void (WINAPI* flush)();
    if (!tried) {
        tried = true;
        if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            setMode = (int (WINAPI*)(int))GetProcAddress(ux, MAKEINTRESOURCEA(135)); flush = (void (WINAPI*)())GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
    }
    if (setMode && flush) { setMode(light ? 3 : 2); flush(); }                      // 2 = scuro, 3 = chiaro
    if (g_tip) SetWindowTheme(g_tip, light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
}
static void ApplyTheme()
{
    bool light = g_set.theme == 1 || (g_set.theme == 2 && WindowsIsLight());
    const Theme* t = light ? &kLight : &kDark;
    if (t == g_theme && g_hwnd) return;
    g_theme = t; ControlsTheme(light);
    if (!g_hwnd) return;
    BOOL dark = !light; DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DrawThemeChanged(); InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---- misura e posizione ----
static RECT WorkArea(HWND hwnd)
{
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi); return mi.rcWork;
}
static float MinWidth()
{
    if (g_minDrives != g_state.ndrive) { g_minW = PanelFitWidth(g_state); g_minDrives = g_state.ndrive; }
    return g_minW;
}
// l'altezza segue il contenuto (cambia quando arriva l'elenco dei dischi); un widget appoggiato al bordo basso dello schermo ci resta
static void FitHeight(HWND hwnd)
{
    RECT rc; GetWindowRect(hwnd, &rc);
    int h = Px(hwnd, PanelHeight(g_state)), w = Px(hwnd, (float)g_set.width);
    if (rc.bottom - rc.top == h && rc.right - rc.left == w) return;
    RECT work = WorkArea(hwnd); int y = rc.top;
    if (work.bottom - rc.bottom <= Px(hwnd, 24) && rc.bottom <= work.bottom) y = rc.bottom - h;
    SetWindowPos(hwnd, nullptr, rc.left, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
// adatta: la larghezza più stretta che mostra tutto; un widget appoggiato al bordo destro ci resta
static void DoFit(HWND hwnd)
{
    RECT rc; GetWindowRect(hwnd, &rc); RECT work = WorkArea(hwnd);
    bool right = work.right - rc.right <= Px(hwnd, 24) && rc.right <= work.right;
    g_set.width = (int)MinWidth();
    int w = Px(hwnd, (float)g_set.width), h = Px(hwnd, PanelHeight(g_state)), x = right ? rc.right - w : rc.left;
    SetWindowPos(hwnd, nullptr, x, rc.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SettingsSave();
}
// in un angolo dell'area di lavoro dello schermo su cui sta (mai sopra la barra delle applicazioni), a 12 unità dai bordi
static void MoveToCorner(HWND hwnd, bool right, bool bottom)
{
    RECT rc; GetWindowRect(hwnd, &rc); RECT work = WorkArea(hwnd);
    int w = rc.right - rc.left, h = rc.bottom - rc.top, m = Px(hwnd, 12);
    SetWindowPos(hwnd, nullptr, right ? work.right - w - m : work.left + m, bottom ? work.bottom - h - m : work.top + m, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SettingsSave();
}
static void SetTopmost(HWND hwnd, bool on)
{
    g_set.topmost = on; g_ui.pinned = on;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, FALSE); SettingsSave();
}
static void ShowPanel(HWND hwnd, bool show)
{
    g_set.visible = show;
    ShowWindow(hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) InvalidateRect(hwnd, nullptr, FALSE);
    SettingsSave();
}

// ---- tooltip: una zona per ogni cosa che ha qualcosa da spiegare; il testo si fa quando serve, coi valori di quel momento ----
static void TipsLayout(HWND hwnd)
{
    if (!g_tip) return;
    for (int i = 0; i < g_nrg; i++) { TTTOOLINFOW ti = { TTTOOLINFOW_V2_SIZE }; ti.hwnd = hwnd; ti.uId = i + 1; SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti); }
    RECT rc; GetClientRect(hwnd, &rc); float sc = Scale(hwnd);
    g_nrg = PanelRegions(g_state, rc.right / sc, g_rg, 48);
    for (int i = 0; i < g_nrg; i++) {
        TTTOOLINFOW ti = { TTTOOLINFOW_V2_SIZE }; ti.uFlags = TTF_SUBCLASS; ti.hwnd = hwnd; ti.uId = i + 1; ti.lpszText = LPSTR_TEXTCALLBACKW;
        SetRect(&ti.rect, (int)(g_rg[i].x * sc), (int)(g_rg[i].y * sc), (int)((g_rg[i].x + g_rg[i].w) * sc), (int)((g_rg[i].y + g_rg[i].h) * sc));
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}
static void TipsCreate(HWND hwnd)
{
    InitCommonControls();
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd, nullptr, g_inst, nullptr);
    if (!g_tip) return;
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, Px(hwnd, 460)); SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    SetWindowTheme(g_tip, g_theme->light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
    TipsLayout(hwnd);
}
static int RegionAt(HWND hwnd, int px, int py)
{
    float sc = Scale(hwnd), x = px / sc, y = py / sc;
    for (int i = 0; i < g_nrg; i++) if (x >= g_rg[i].x && x < g_rg[i].x + g_rg[i].w && y >= g_rg[i].y && y < g_rg[i].y + g_rg[i].h) return g_rg[i].id;
    return 0;
}
static const Program* ProgramOf(int region)
{
    if (region >= RG_TOPCPU && region < RG_TOPCPU + g_state.nTopCpu) return &g_state.topCpu[region - RG_TOPCPU];
    if (region >= RG_TOPRAM && region < RG_TOPRAM + g_state.nTopRam) return &g_state.topRam[region - RG_TOPRAM];
    return nullptr;
}
// il testo di una zona; vuoto = nessun tooltip
static void TipText(int region, Lb& b)
{
    const State& s = g_state;
    static const wchar_t* btn[NBTN] = { L"Fit the size to the content", L"Move to a corner of the screen", L"Always on top (click to toggle)", L"Hide (stays in the tray)" };
    if (region >= RG_BTN && region < RG_BTN + NBTN) { b.t(btn[region - RG_BTN]); return; }
    switch (region) {
    case RG_POWER: b.t(L"Estimated total PC power: CPU + graphics card + about 30 W for the rest"); return;
    case RG_CPU_TEMP:
        // perché manca: i registri della CPU li legge il processo dei sensori, da amministratore e col driver PawnIO
        if (Has(s.cpuTemp)) b.t(L"CPU temperature: also the thin line in the graph, on the same 0 to 100 scale as the load.");
        else if (!(s.sensorFlags & SF_ALIVE)) b.t(L"CPU temperature and power come from Pulse's sensors process (Pulse2.Sensors), which is not running.");
        else if (!(s.sensorFlags & SF_PAWNIO) && !(s.sensorFlags & SF_ELEVATED)) b.t(L"CPU temperature and power need the PawnIO driver and the sensors process running as administrator.\nInstall PawnIO from pawnio.eu (or: winget install namazso.PawnIO).");
        else if (!(s.sensorFlags & SF_PAWNIO)) b.t(L"CPU temperature and power need the PawnIO driver.\nInstall it from pawnio.eu (or: winget install namazso.PawnIO), then restart Pulse.");
        else if (!(s.sensorFlags & SF_ELEVATED)) b.t(L"CPU temperature not available.\nThe sensors process must run as administrator to read it.");
        else b.t(L"CPU temperature not available: the processor's sensors could not be read.");
        return;
    case RG_CORES: {
        int phys = 0; for (int i = 0; i < s.ncore; i++) if (i == 0 || s.phys[i] != s.phys[i - 1]) phys++;
        b.t(L"Sum of the loads of the ");
        if (phys < s.ncore) b.t(L"logical cores (").i(s.ncore).t(L", on ").i(phys).t(L" physical cores)"); else b.t(L"cores (").i(s.ncore).c(L')');
        b.t(L": how many full cores the current work is worth.\nUnder the graph, ");
        b.t(phys < s.ncore ? L"one bar per logical core, those of the same physical core side by side" : L"one bar per core");
        b.t(L": many medium bars mean parallel work, a single full one a single thread.");
        if ((s.sensorFlags & SF_ALIVE) && (s.sensorFlags & (SF_ELEVATED | SF_FAKE)) && !(s.sensorFlags & SF_NO_SAMPLING))
            b.t(L"\nColours: the share of each core used by the Top CPU programs above half a core, up to three (the small square next to the name), sampled by the kernel 100 times a second, only while there is one; the rest in the CPU colour.");
        else b.t(L"\nTo see in colour which Top CPU programs use the cores, the sensors process must run as administrator.");
        if (s.clockKind == 3) b.t(L"\nClock of the fastest core.");
        else if (s.clockKind == 2) b.t(L"\nClock: the average speed of the cores at work, as in Task Manager.");
        else if (s.clockKind == 1) b.t(L"\nClock reported by Windows (the base clock on many processors).");
        return; }
    case RG_RAM:
        if (s.ramParts) {
            b.t(L"In use ").f1(s.ramInUse).t(L" GB: processes, drivers and the system.\nModified ").f1(s.ramModified).t(L" GB: to be written to disk before it can be reused.");
            b.t(L"\nCached ").f1(s.ramStandby).t(L" GB: data and programs kept ready, given back by Windows as soon as it's needed.\nFree ").f1(s.ramFree).t(L" GB.");
            b.t(L"\nAvailable ").f1(s.ramAvail).t(L" GB: cached plus free, what programs can still take.");
        } else b.t(L"Used ").f1(s.ramUsed).t(L" GB, available ").f1(s.ramAvail).t(L" GB.");
        if (s.commitLimit > 0) b.t(L"\nCommitted ").f1(s.commit).t(L" of ").f1(s.commitLimit).t(L" GB: what programs have asked for, against the limit of RAM plus page file.");
        if (s.pageTotal > 0) b.t(L"\nPage file: ").f2(s.pageUsed).t(L" GB in use of ").f1(s.pageTotal).t(L" GB."); else b.t(L"\nNo page file.");
        return;
    case RG_GPU:
        if (!s.gpuOk) { b.t(L"No graphics card read: this build reads NVIDIA cards only."); return; }
        b.t(s.gpuName);
        if (s.gpuClock > 0) b.t(L"\nCore clock ").r(s.gpuClock).t(L" MHz");
        if (s.gpuMemClock > 0) b.t(L"\nMemory clock ").r(s.gpuMemClock).t(L" MHz");
        return;
    }
    if (const Program* p = ProgramOf(region)) {
        if (region < RG_TOPRAM) {
            b.f2(p->cpu * s.ncore / 100).t(L" cores: how many full cores this program is worth");
            if (p->count > 1) b.c(L'\n').i(p->count).t(L" processes, added up");
        } else if (p->count > 1) b.i(p->count).t(L" processes, their memory added up: what they share counts more than once");
    }
}

// ---- il menu di un programma: cartella, proprietà, termina, cerca ----
static bool ProgramPath(DWORD pid, wchar_t* path)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return false;
    DWORD n = MAX_PATH; bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0; CloseHandle(h); return ok;
}
// il nome come sta negli elenchi: senza .exe, accorciato come lì
static void ListedName(const wchar_t* exe, wchar_t* name)
{
    lstrcpynW(name, exe, PROGNAME); int n = lstrlenW(name);
    if (n > 4 && !lstrcmpiW(name + n - 4, L".exe")) name[n - 4] = 0;
}
static void ProgramMenu(HWND hwnd, const Program& p)
{
    g_menuProg = p;
    HMENU menu = CreatePopupMenu(); Sb end;
    if (p.count > 1) end.t(L"End all ").i(p.count).t(L" processes"); else end.t(L"End process");
    AppendMenuW(menu, MF_STRING, ID_P_OPEN, L"Open file location"); AppendMenuW(menu, MF_STRING, ID_P_PROPS, L"Properties");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, ID_P_END, end.s);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, ID_P_SEARCH, L"Search online");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd); TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr); PostMessageW(hwnd, WM_NULL, 0, 0); DestroyMenu(menu);
}
// una riga è un programma: la conferma dice quanti processi chiude. Si chiudono quelli che hanno quel nome adesso; per un
// programma di un processo solo, quel processo e solo se ha ancora quel nome (il numero può essere passato a un altro)
static void ProgramEnd(HWND hwnd, const Program& p)
{
    Lb q; q.t(L"End ").t(p.name);
    if (p.count > 1) q.t(L"? All its ").i(p.count).t(L" processes will be closed."); else q.t(L" (PID ").i(p.pid).t(L")?");
    if (MessageBoxW(hwnd, q.s, L"Confirm", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) != IDYES) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e = { sizeof(e) }; int failed = 0, found = 0; DWORD error = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        wchar_t name[PROGNAME]; ListedName(e.szExeFile, name);
        if (lstrcmpiW(name, p.name) || (p.count == 1 && e.th32ProcessID != p.pid)) continue;
        found++;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, e.th32ProcessID);
        if (!h || !TerminateProcess(h, 1)) { failed++; if (!error) error = GetLastError(); }
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    if (failed) {
        Lb m; if (found > 1) m.t(L"Couldn't end ").i(failed).t(L" of ").i(found).t(L" processes"); else m.t(L"Couldn't end the process");
        m.t(error == ERROR_ACCESS_DENIED ? L": access denied." : L".");
        MessageBoxW(hwnd, m.s, L"Error", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}
static void ProgramCommand(HWND hwnd, int id)
{
    const Program& p = g_menuProg; wchar_t path[MAX_PATH];
    if (!p.name[0]) return;
    if (id == ID_P_END) { ProgramEnd(hwnd, p); return; }
    if (id == ID_P_SEARCH) {                                                         // il browser predefinito, col nome del processo nella ricerca
        Lb url; url.t(L"https://www.google.com/search?q=");
        char utf8[160]; int n = WideCharToMultiByte(CP_UTF8, 0, p.name, -1, utf8, sizeof(utf8), nullptr, nullptr);
        for (int i = 0; i < n - 1; i++) {
            unsigned char ch = (unsigned char)utf8[i];
            if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_' || ch == '.') url.c((wchar_t)ch);
            else { static const wchar_t hex[] = L"0123456789ABCDEF"; url.c(L'%').c(hex[ch >> 4]).c(hex[ch & 15]); }
        }
        url.t(L"%20process%20windows");
        ShellExecuteW(hwnd, L"open", url.s, nullptr, nullptr, SW_SHOWNORMAL); return;
    }
    if (!ProgramPath(p.pid, path)) return;                                           // un processo di sistema che non si lascia aprire: niente
    if (id == ID_P_OPEN) { Lb arg; arg.t(L"/select,\"").t(path).c(L'"'); ShellExecuteW(hwnd, L"open", L"explorer.exe", arg.s, nullptr, SW_SHOWNORMAL); }
    else if (id == ID_P_PROPS) {
        SHELLEXECUTEINFOW info = { sizeof(info) }; info.fMask = SEE_MASK_INVOKEIDLIST; info.hwnd = hwnd; info.lpVerb = L"properties"; info.lpFile = path; info.nShow = SW_SHOW;
        ShellExecuteExW(&info);
    }
}

// ---- tray ----
static void TrayAdd(HWND hwnd)
{
    NOTIFYICONDATAW n = { sizeof(n) };
    n.hWnd = hwnd; n.uID = 1; n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; n.uCallbackMessage = WM_TRAY;
    n.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpyW(n.szTip, L"Pulse 2");
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &n);
}
static void TrayRemove(HWND hwnd)
{
    NOTIFYICONDATAW n = { sizeof(n) }; n.hWnd = hwnd; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n);
}
// il tooltip dell'icona dice i tre carichi: col pannello nascosto è l'unico posto dove si vedono
static void TrayTip(HWND hwnd)
{
    static wchar_t last[64];
    Sb b; b.t(L"Pulse 2 \x2014 CPU ").r(g_state.cpu).t(L"% \x00B7 RAM ").r(g_state.ramPct).c(L'%');
    if (g_state.gpuOk) b.t(L" \x00B7 GPU ").r(g_state.gpu).c(L'%');
    if (!lstrcmpW(last, b.s)) return;
    lstrcpynW(last, b.s, 64);
    NOTIFYICONDATAW n = { sizeof(n) }; n.hWnd = hwnd; n.uID = 1; n.uFlags = NIF_TIP | NIF_SHOWTIP; lstrcpynW(n.szTip, b.s, 64);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}
static void Popup(HWND hwnd, HMENU menu, int x, int y)
{
    SetForegroundWindow(hwnd);                                                       // senza, il menu non si chiude cliccando altrove
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}
static void TrayMenu(HWND hwnd, int x, int y)
{
    HMENU menu = CreatePopupMenu(), theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING | (g_set.theme == 0 ? MF_CHECKED : 0), ID_DARK, L"Dark");
    AppendMenuW(theme, MF_STRING | (g_set.theme == 1 ? MF_CHECKED : 0), ID_LIGHT, L"Light");
    AppendMenuW(theme, MF_STRING | (g_set.theme == 2 ? MF_CHECKED : 0), ID_FOLLOW, L"Follow Windows");
    AppendMenuW(menu, MF_STRING, ID_SHOW, IsWindowVisible(hwnd) ? L"Hide" : L"Show");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)theme, L"Theme");
    AppendMenuW(menu, MF_STRING | (g_set.topmost ? MF_CHECKED : 0), ID_TOPMOST, L"Always on top");
    AppendMenuW(menu, MF_STRING | (Autostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    Popup(hwnd, menu, x, y);
}
// la scelta dell'angolo: quattro frecce sotto il bottone, come in Pulse 1.x. Si chiude scegliendo, cliccando altrove, con Esc o
// quando il mouse lascia il pannello
static void SetPicker(HWND hwnd, bool open)
{
    if (g_ui.picker == open) return;
    g_ui.picker = open; g_ui.hotArrow = -1; InvalidateRect(hwnd, nullptr, FALSE);
}
static void Command(HWND hwnd, int id)
{
    switch (id) {
    case ID_SHOW: ShowPanel(hwnd, !IsWindowVisible(hwnd)); break;
    case ID_DARK: case ID_LIGHT: case ID_FOLLOW: g_set.theme = id - ID_DARK; ApplyTheme(); SettingsSave(); break;
    case ID_TOPMOST: SetTopmost(hwnd, !g_set.topmost); break;
    case ID_AUTOSTART: SetAutostart(!Autostart()); break;
    case ID_EXIT: DestroyWindow(hwnd); break;
    case ID_P_OPEN: case ID_P_PROPS: case ID_P_END: case ID_P_SEARCH: ProgramCommand(hwnd, id); break;
    default: if (id >= ID_CORNER && id < ID_CORNER + 4) MoveToCorner(hwnd, (id - ID_CORNER) & 1, (id - ID_CORNER) >= 2);
    }
}
static void Button(HWND hwnd, int k)
{
    switch (k) {
    case BTN_FIT: DoFit(hwnd); break;
    case BTN_CORNER: SetPicker(hwnd, !g_ui.picker); break;
    case BTN_PIN: SetTopmost(hwnd, !g_set.topmost); break;
    case BTN_HIDE: ShowPanel(hwnd, false); break;
    }
}

// il bottone della testata sotto un punto dell'area di disegno (in pixel), -1 se non c'è
static int ButtonAt(HWND hwnd, int px, int py)
{
    RECT rc; GetClientRect(hwnd, &rc); float sc = Scale(hwnd), x = px / sc, y = py / sc, W = rc.right / sc;
    if (y < 2 || y >= HEADER_H - 2 || x < BtnX(W, 0) || x >= BtnX(W, NBTN)) return -1;
    return (int)((x - BtnX(W, 0)) / BTN_W);
}
// la freccia della scelta dell'angolo sotto un punto, -1 se non c'è
static int ArrowAt(HWND hwnd, int px, int py)
{
    if (!g_ui.picker) return -1;
    RECT rc; GetClientRect(hwnd, &rc); float sc = Scale(hwnd), x = px / sc, y = py / sc, W = rc.right / sc;
    for (int k = 0; k < 4; k++) { float cx, cy; PickerCell(W, k, cx, cy); if (x >= cx && x < cx + PICK_CELL && y >= cy && y < cy + PICK_CELL) return k; }
    return -1;
}
// che cosa c'è sotto il mouse: un bottone, una freccia, una riga di programma. Si ridisegna solo se cambia
static void SetHover(HWND hwnd, int button, int region, int arrow)
{
    if (region < RG_TOPCPU || !ProgramOf(region) || arrow >= 0) region = 0;         // si evidenziano solo le righe che hanno un programma
    if (button == g_ui.hot && region == g_ui.hotRegion && arrow == g_ui.hotArrow) return;
    g_ui.hot = button; g_ui.hotRegion = region; g_ui.hotArrow = arrow; InvalidateRect(hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbar && msg) { TrayAdd(hwnd); return 0; }                    // Esplora risorse è ripartito: l'icona va rimessa
    if (msg == g_msgShow && msg) { ShowPanel(hwnd, true); return 0; }               // un secondo Pulse 2 è stato avviato: si mostra questo
    switch (msg) {
    case WM_NCHITTEST: {                                                            // i lati allargano, la testata trascina, i bottoni e il corpo sono del pannello
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hwnd, &p);
        RECT rc; GetClientRect(hwnd, &rc); int edge = Px(hwnd, 6);
        if (p.x < edge) return HTLEFT;
        if (p.x >= rc.right - edge) return HTRIGHT;
        if (p.y < Px(hwnd, HEADER_H)) return ButtonAt(hwnd, p.x, p.y) >= 0 ? HTCLIENT : HTCAPTION;
        return HTCLIENT; }
    case WM_NCLBUTTONDBLCLK: return 0;                                              // il doppio clic sulla testata non ingrandisce niente
    case WM_NCRBUTTONUP: return 0;
    case WM_NCMOUSEMOVE: SetHover(hwnd, -1, 0, -1); break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* m = (MINMAXINFO*)lp; int h = Px(hwnd, PanelHeight(g_state));
        m->ptMinTrackSize.x = Px(hwnd, MinWidth()); m->ptMaxTrackSize.x = Px(hwnd, 1600); m->ptMinTrackSize.y = m->ptMaxTrackSize.y = h;
        return 0; }
    case WM_MOVING: {                                                               // trascinato vicino a un bordo dell'area di lavoro, il pannello si ferma a 12 unità: lo stesso margine degli angoli
        RECT* r = (RECT*)lp; MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& w = mi.rcWork; int m = Px(hwnd, 12), snap = Px(hwnd, 10), dx = 0, dy = 0;
        if (abs(r->left - (w.left + m)) <= snap) dx = w.left + m - r->left; else if (abs(r->right - (w.right - m)) <= snap) dx = w.right - m - r->right;
        if (abs(r->top - (w.top + m)) <= snap) dy = w.top + m - r->top; else if (abs(r->bottom - (w.bottom - m)) <= snap) dy = w.bottom - m - r->bottom;
        OffsetRect(r, dx, dy);
        return TRUE; }
    case WM_WTSSESSION_CHANGE:                                                      // sessione bloccata: nessuno guarda
        if (wp == WTS_SESSION_LOCK) g_locked = true; else if (wp == WTS_SESSION_UNLOCK) { g_locked = false; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const POWERBROADCAST_SETTING* ps = (const POWERBROADCAST_SETTING*)lp;
            if (ps && IsEqualGUID(ps->PowerSetting, kDisplayState) && ps->DataLength >= 4) { g_displayOff = *(const DWORD*)ps->Data == 0; if (!g_displayOff) InvalidateRect(hwnd, nullptr, FALSE); }
        }
        return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        MetricsTick(g_state); SlowCopy(g_state); SensorsExchange(g_state); FitHeight(hwnd);
        static ULONGLONG minute; if (g_state.uptimeSec / 60 != minute) { minute = g_state.uptimeSec / 60; TipsLayout(hwnd); }   // la riga della testata si allunga coi minuti
        // nascosto nel tray, a schermo bloccato o spento legge, così il minuto è pronto quando torna, ma non disegna
        if (IsWindowVisible(hwnd) && !g_locked && !g_displayOff) InvalidateRect(hwnd, nullptr, FALSE);
        if (g_ticks++ % 5 == 0) TrayTip(hwnd);
        if (g_seconds && g_ticks >= (unsigned)g_seconds) DestroyWindow(hwnd);
        return 0; }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp)) { g_set.width = (int)(LOWORD(lp) / Scale(hwnd) + .5f); DrawResize(hwnd); TipsLayout(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp; int i = (int)di->hdr.idFrom - 1; Lb b;
            if (i >= 0 && i < g_nrg) TipText(g_rg[i].id, b);
            lstrcpynW(g_tipText, b.s, 1100); di->lpszText = g_tipText;
        }
        return 0; }
    case WM_RBUTTONUP:                                                              // tasto destro su una riga di Top CPU o Top RAM
        if (const Program* p = ProgramOf(RegionAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))) ProgramMenu(hwnd, *p);
        return 0;
    case WM_EXITSIZEMOVE: SettingsSave(); return 0;
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        DrawResize(hwnd); FitHeight(hwnd); return 0; }
    case WM_SETTINGCHANGE: if (g_set.theme == 2) ApplyTheme(); return 0;            // Windows è passato da chiaro a scuro, o viceversa
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        DrawPaint(hwnd, g_state, g_ui);
        EndPaint(hwnd, &ps);
        return 0; }
    case WM_MOUSEMOVE:
        if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; g_tracking = TrackMouseEvent(&t) != 0; }
        { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); SetHover(hwnd, ButtonAt(hwnd, x, y), RegionAt(hwnd, x, y), ArrowAt(hwnd, x, y)); }
        return 0;
    case WM_MOUSELEAVE: g_tracking = false; SetHover(hwnd, -1, 0, -1); if (GetCapture() != hwnd) SetPicker(hwnd, false); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) SetPicker(hwnd, false); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) SetPicker(hwnd, false); break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        g_pressedArrow = ArrowAt(hwnd, x, y); g_pressed = g_pressedArrow >= 0 ? -1 : ButtonAt(hwnd, x, y);
        if (g_ui.picker && g_pressedArrow < 0 && g_pressed != BTN_CORNER) SetPicker(hwnd, false);   // un clic altrove chiude la scelta dell'angolo
        if (g_pressed >= 0 || g_pressedArrow >= 0) SetCapture(hwnd);
        return 0; }
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), k = ButtonAt(hwnd, x, y), arrow = ArrowAt(hwnd, x, y), was = g_pressed, wasArrow = g_pressedArrow;
        g_pressed = g_pressedArrow = -1; if (GetCapture() == hwnd) ReleaseCapture();
        if (wasArrow >= 0) { if (arrow == wasArrow) { SetPicker(hwnd, false); MoveToCorner(hwnd, (arrow & 1) != 0, arrow >= 2); } }
        else if (k >= 0 && k == was) Button(hwnd, k);
        return 0; }
    case WM_TRAY:                                                                   // NOTIFYICON_VERSION_4: l'evento è nella parola bassa, il punto in wParam
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: ShowPanel(hwnd, !IsWindowVisible(hwnd)); break;
        case WM_CONTEXTMENU: TrayMenu(hwnd, GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
        }
        return 0;
    case WM_COMMAND: Command(hwnd, LOWORD(wp)); return 0;
    case WM_DESTROY: WTSUnRegisterSessionNotification(hwnd); TrayRemove(hwnd); SettingsSave(); SlowStop(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- quanto costa un fotogramma: gli stessi dati a ogni corsa (grafici mossi, tre programmi a colori, elenchi pieni), n disegni di
// fila, e il tempo di CPU di tutto il processo, che comprende i thread con cui Direct2D disegna via software ----
static ULONGLONG ProcessCpu100ns()
{
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
static void BenchFill(State& s)
{
    unsigned seed = 0x9E3779B9u;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed >> 8) * (1.0f / 16777216.0f); };
    float cpu = 20, ram = 30, gpu = 10;
    for (int i = 0; i < HIST; i++) {
        cpu = Clampf(cpu + (rnd() - .5f) * 18, 3, 90); ram = Clampf(ram + (rnd() - .5f) * 2, 20, 45); gpu = Clampf(gpu + (rnd() - .5f) * 25, 0, 95);
        s.hCpu[i] = cpu; s.hCt[i] = 50 + cpu * .4f; s.hRam[i] = ram; s.hGpu[i] = gpu; s.hGt[i] = 45 + gpu * .3f;
        s.hDown[i] = rnd() < .2f ? rnd() * 3e7f : rnd() * 2e5f; s.hUp[i] = rnd() * 4e5f; s.hDr[i] = rnd() < .2f ? rnd() * 2e8f : rnd() * 3e6f; s.hDw[i] = rnd() * 4e7f;
    }
    s.cpu = cpu; s.cpuTemp = 62; s.cpuWatt = 95; s.ramPct = ram; s.gpuOk = true; s.gpu = gpu; s.gpuTemp = 51; s.gpuWatt = 60; s.vramUsed = 4.2f; s.vramTotal = 15.9f;
    s.down = s.hDown[HIST - 1]; s.up = s.hUp[HIST - 1]; s.dr = s.hDr[HIST - 1]; s.dw = s.hDw[HIST - 1];
    s.coresInUse = 0;
    for (int i = 0; i < s.ncore; i++) { s.cores[i] = rnd() * rnd(); s.coresInUse += s.cores[i]; for (int k = 0; k < 3; k++) s.share[i][k] = (i % 5 == k) ? .6f : 0; }
    static const wchar_t* names[NTOP] = { L"renderer", L"compiler", L"indexer", L"msedgewebview2", L"System", L"explorer" };
    for (int i = 0; i < NTOP; i++) {
        Program& a = s.topCpu[i]; lstrcpyW(a.name, names[i]); a.count = i < 2 ? 3 - i : 1 + i * 7; a.cpu = 6.f / (i + 1); a.ramMb = 400; a.slot = i < 3 ? i : -1;
        Program& b = s.topRam[i]; lstrcpyW(b.name, names[NTOP - 1 - i]); b.count = 1 + i * 9; b.cpu = 1; b.ramMb = 6000.f / (i + 1); b.slot = -2;
    }
    s.nTopCpu = s.nTopRam = NTOP;
    if (!s.ndrive) { s.ndrive = 3; for (int i = 0; i < 3; i++) { s.drives[i].name[0] = (wchar_t)(L'C' + i); s.drives[i].name[1] = L':'; s.drives[i].name[2] = 0; s.drives[i].freeGb = 130.f + 55 * i; s.drives[i].totalGb = 440.f + 270 * (i == 1); } }
}
static void Bench(HWND hwnd, int frames)
{
    Sleep(2500); MetricsTick(g_state); SlowCopy(g_state); BenchFill(g_state);
    SetWindowPos(hwnd, nullptr, 0, 0, Px(hwnd, (float)g_set.width), Px(hwnd, PanelHeight(g_state)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < 5; i++) DrawPaint(hwnd, g_state, g_ui);                     // i primi fotogrammi scaldano le cache: non contano
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    ULONGLONG cpu = ProcessCpu100ns();
    for (int i = 0; i < frames; i++) {
        g_state.uptimeSec += 61; g_state.cpu += .1f; g_state.down += 1000;                // qualche testo cambia a ogni fotogramma, come nel pannello vero
        DrawPaint(hwnd, g_state, g_ui);
    }
    cpu = ProcessCpu100ns() - cpu; QueryPerformanceCounter(&b);
    char line[160]; int wallUs = (int)((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart / frames), cpuUs = (int)(cpu / 10 / frames);
    wsprintfA(line, "bench frames=%d width=%d cpu_us_per_frame=%d wall_us_per_frame=%d", frames, g_set.width, cpuUs, wallUs); LogLine(line);
}

// due letture a un secondo l'una dall'altra: quanto basta perché percentuali e velocità abbiano un valore
static void ReadOnce()
{
    MetricsInit(g_state); SlowOnce();
    Sleep(1000);
    MetricsTick(g_state); SlowOnce(); SlowCopy(g_state);
}

static int Shot(const wchar_t* arg)
{
    wchar_t path[MAX_PATH]; ReadPath(arg, path);
    ReadOnce();
    if (g_demoData) BenchFill(g_state);                                             // i dati finti di --bench: grafici mossi anche a PC fermo
    int w = (int)(g_set.width * g_shotDpi / 96.f + .5f), h = (int)(PanelHeight(g_state) * g_shotDpi / 96.f + .5f);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return 2;
    SelectObject(dc, bmp);
    Ui ui = { -1, true, 0, false, -1 };
    if (g_demoUi) { ui.hot = BTN_FIT; ui.hotRegion = RG_TOPCPU + 1; ui.picker = true; ui.hotArrow = 1; }
    bool ok = DrawToDc(dc, w, h, (float)g_shotDpi, g_state, ui);
    GdiFlush();
    if (!ok) return 3;
    BITMAPFILEHEADER fh = {}; DWORD size = (DWORD)w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + size;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 4;
    DWORD wr; WriteFile(f, &fh, sizeof(fh), &wr, nullptr); WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr); WriteFile(f, bits, size, &wr, nullptr);
    CloseHandle(f); return 0;
}

template <int N> static void DumpLine(HANDLE f, const SbT<N>& b)
{
    char utf8[3400]; int n = WideCharToMultiByte(CP_UTF8, 0, b.s, b.n, utf8, sizeof(utf8) - 2, nullptr, nullptr);
    utf8[n++] = '\r'; utf8[n++] = '\n'; DWORD w; WriteFile(f, utf8, n, &w, nullptr);
}
static int Dump(const wchar_t* arg)
{
    wchar_t path[MAX_PATH]; ReadPath(arg, path);
    ReadOnce();
    const State& s = g_state;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 4;
    Sb b;
    DumpLine(f, b.clear().t(L"cpu_percent ").f2(s.cpu));
    DumpLine(f, b.clear().t(L"cores_logical ").i(s.ncore));
    DumpLine(f, b.clear().t(L"cores_in_use ").f2(s.coresInUse));
    { int phys = 0; for (int i = 0; i < s.ncore; i++) if (i == 0 || s.phys[i] != s.phys[i - 1]) phys++; DumpLine(f, b.clear().t(L"cores_physical ").i(phys)); }
    DumpLine(f, b.clear().t(L"cpu_ghz ").f2(s.cpuGhz));
    DumpLine(f, b.clear().t(L"ram_total_gb ").f2(s.ramTotal));
    DumpLine(f, b.clear().t(L"ram_avail_gb ").f2(s.ramAvail));
    DumpLine(f, b.clear().t(L"ram_parts ").i(s.ramParts).t(L" in_use ").f2(s.ramInUse).t(L" modified ").f2(s.ramModified).t(L" standby ").f2(s.ramStandby).t(L" free ").f2(s.ramFree));
    DumpLine(f, b.clear().t(L"gpu_ok ").i(s.gpuOk).t(L" load ").r(s.gpu).t(L" temp ").r(Has(s.gpuTemp) ? s.gpuTemp : 0).t(L" watt ").f1(Has(s.gpuWatt) ? s.gpuWatt : 0)
                 .t(L" vram ").f2(s.vramUsed).t(L" / ").f2(s.vramTotal).t(L" clock ").r(s.gpuClock).t(L" memclock ").r(s.gpuMemClock).t(L" name ").t(s.gpuName));
    DumpLine(f, b.clear().t(L"net_down_bps ").r(s.down).t(L" up_bps ").r(s.up));
    DumpLine(f, b.clear().t(L"disk_read_bps ").r(s.dr).t(L" write_bps ").r(s.dw));
    DumpLine(f, b.clear().t(L"adapter ").t(s.adapter).t(L" local ").t(s.localIp).t(L" ping_ms ").r(Has(s.pingMs) ? s.pingMs : 0).t(Has(s.pingMs) ? L"" : L" (none)"));
    DumpLine(f, b.clear().t(L"uptime_s ").i((long long)s.uptimeSec));
    DumpLine(f, b.clear().t(L"fit_width ").r(PanelFitWidth(s)).t(L" height ").r(PanelHeight(s)));
    for (int i = 0; i < s.ndrive; i++) DumpLine(f, b.clear().t(L"drive ").t(s.drives[i].name).t(L" free_gb ").f2(s.drives[i].freeGb).t(L" total_gb ").f2(s.drives[i].totalGb));
    for (int i = 0; i < s.nTopCpu; i++) DumpLine(f, b.clear().t(L"top_cpu ").t(s.topCpu[i].name).t(L" x").i(s.topCpu[i].count).t(L" cpu ").f2(s.topCpu[i].cpu).t(L" pid ").i(s.topCpu[i].pid));
    for (int i = 0; i < s.nTopRam; i++) DumpLine(f, b.clear().t(L"top_ram ").t(s.topRam[i].name).t(L" x").i(s.topRam[i].count).t(L" mb ").r(s.topRam[i].ramMb).t(L" pid ").i(s.topRam[i].pid));
    DumpLine(f, b.clear().t(L"clock_kind ").i(s.clockKind).t(L" commit_gb ").f2(s.commit).t(L" of ").f2(s.commitLimit).t(L" pagefile_gb ").f2(s.pageUsed).t(L" of ").f2(s.pageTotal));
    // le zone e i loro tooltip, una riga l'una (gli a capo diventano barre)
    g_nrg = PanelRegions(s, (float)g_set.width, g_rg, 48);
    for (int i = 0; i < g_nrg; i++) {
        Lb t, line; TipText(g_rg[i].id, t);
        for (int k = 0; k < t.n; k++) if (t.s[k] == L'\n') t.s[k] = L'|';
        line.t(L"tip ").i(g_rg[i].id).t(L" at ").r(g_rg[i].x).c(L',').r(g_rg[i].y).t(L" size ").r(g_rg[i].w).c(L'x').r(g_rg[i].h).t(L": ").t(t.s);
        DumpLine(f, line);
    }
    CloseHandle(f); return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;
    g_nosave = Find(cmd, L"--nosave") != nullptr; g_noact = Find(cmd, L"--noactivate") != nullptr; g_demoUi = Find(cmd, L"--demo-ui") != nullptr; g_demoData = Find(cmd, L"--demo-data") != nullptr;
    bool tool = Find(cmd, L"--shot") || Find(cmd, L"--dump");
    if (tool) g_nosave = true;

    // un'istanza sola: la seconda dice alla prima di mostrarsi ed esce
    g_msgShow = RegisterWindowMessageW(L"Pulse2.Show");
    if (!g_nosave) {
        CreateMutexW(nullptr, TRUE, L"Pulse2.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED) {
            if (HWND other = FindWindowW(L"Pulse2", nullptr)) PostMessageW(other, g_msgShow, 0, 0);
            return 0;
        }
    }

    LoadFonts();
    SettingsLoad();
    bool posGiven = false; int x = 0, y = 0;
    if (const wchar_t* p = Find(cmd, L"--width")) { g_set.width = ReadInt(p); if (g_set.width < 300) g_set.width = 300; if (g_set.width > 1600) g_set.width = 1600; }
    if (const wchar_t* p = Find(cmd, L"--pos")) { x = ReadInt(p); y = ReadInt(p); posGiven = true; }
    if (const wchar_t* p = Find(cmd, L"--theme")) { p = SkipSp(p); g_set.theme = *p == L'l' ? 1 : 0; }
    if (const wchar_t* p = Find(cmd, L"--seconds")) g_seconds = ReadInt(p);
    if (const wchar_t* p = Find(cmd, L"--dpi")) { g_shotDpi = ReadInt(p); if (g_shotDpi < 96 || g_shotDpi > 384) g_shotDpi = 96; }
    ApplyTheme();
    if (const wchar_t* p = Find(cmd, L"--shot")) return Shot(p);
    if (const wchar_t* p = Find(cmd, L"--dump")) return Dump(p);

    MetricsInit(g_state); SlowStart(); SensorsInit();
    g_ui.pinned = g_set.topmost;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"Pulse2";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    // senza cornice e fuori dalla barra delle applicazioni; gli angoli tondi e l'ombra li dà Windows 11
    HWND hwnd = g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | (g_set.topmost ? WS_EX_TOPMOST : 0) | (g_noact ? WS_EX_NOACTIVATE : 0), wc.lpszClassName, L"Pulse 2",
                                         WS_POPUP, 0, 0, g_set.width, (int)PanelHeight(g_state), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    int round = 2; DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE: tondi */, &round, sizeof(round));
    BOOL dark = !g_theme->light; DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    // dove: dagli argomenti, dalle impostazioni se quel punto è ancora su uno schermo, altrimenti in alto a destra dello schermo principale
    if (!posGiven && g_set.have) { POINT p = { g_set.left + 40, g_set.top + 16 }; if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { x = g_set.left; y = g_set.top; posGiven = true; } }
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);   // prima sullo schermo giusto, poi la misura coi suoi DPI
    int w = Px(hwnd, (float)g_set.width), h = Px(hwnd, PanelHeight(g_state));
    if (!posGiven) { RECT work = WorkArea(hwnd); x = work.right - w - Px(hwnd, 12); y = work.top + Px(hwnd, 12); }
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (!DrawCreate(hwnd)) { LogLine("drawing start FAILED"); return 4; }

    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd(hwnd); TipsCreate(hwnd);
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION); RegisterPowerSettingNotification(hwnd, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (g_set.visible) {
        ShowWindow(hwnd, g_noact ? SW_SHOWNOACTIVATE : SW_SHOW);
        DrawPaint(hwnd, g_state, g_ui); ValidateRect(hwnd, nullptr);                // il primo fotogramma subito, senza aspettare WM_PAINT
    }
    { char line[96]; wsprintfA(line, "start first_frame_ms=%d pid=%u", MsSinceProcessStart(), GetCurrentProcessId()); LogLine(line); }
    if (const wchar_t* p = Find(cmd, L"--bench")) { int n = ReadInt(p); Bench(hwnd, n > 0 ? n : 300); DestroyWindow(hwnd); return 0; }
    SetCoalescableTimer(hwnd, 1, 1000, nullptr, 100);                               // Windows può accorparlo ad altri risvegli entro un decimo di secondo

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
