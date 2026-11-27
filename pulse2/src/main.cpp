// Pulse 2: la finestra e quello che le sta intorno. Un pannello senza cornice che si trascina dalla testata e si allarga dai lati
// (l'altezza è sempre quella del contenuto), quattro bottoni in testata (adatta, angolo, in primo piano, nascondi), un'icona nel
// tray col suo menu, le impostazioni in %APPDATA%\Pulse2\settings.txt, un'istanza sola.
//
//   --pos <x> <y>       posizione della finestra            --width <n>      larghezza in unità (predefinita 514)
//   --theme dark|light  tema, al posto di quello salvato    --seconds <n>    si chiude da solo dopo n secondi
//   --noactivate        non prende il fuoco quando si apre  --nosave         prova: non legge né scrive impostazioni, niente istanza unica
//   --shot <file.bmp>   legge i dati per un secondo, disegna un fotogramma fuori schermo, lo salva ed esce (nessuna finestra)
//   --dump <file.txt>   legge i dati per un secondo e li scrive come testo, per confrontarli con i contatori di Windows
#include "app.h"
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>

FontData g_fontData[NFILE];
static State g_state;
static Ui g_ui = { -1, true };
static HWND g_hwnd;
static HINSTANCE g_inst;
static int g_seconds; static unsigned g_ticks;
static bool g_nosave, g_noact;
static int g_pressed = -1; static bool g_tracking;
static float g_minW = 420; static int g_minDrives = -1;
static UINT g_msgTaskbar, g_msgShow;
enum { WM_TRAY = WM_APP + 1 };
enum { ID_SHOW = 1, ID_DARK, ID_LIGHT, ID_FOLLOW, ID_TOPMOST, ID_AUTOSTART, ID_EXIT, ID_CORNER = 10 };   // ID_CORNER + 0..3: alto sx, alto dx, basso sx, basso dx

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
static void ApplyTheme()
{
    bool light = g_set.theme == 1 || (g_set.theme == 2 && WindowsIsLight());
    const Theme* t = light ? &kLight : &kDark;
    if (t == g_theme && g_hwnd) return;
    g_theme = t;
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
static void CornerMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CORNER + 0, L"Top left"); AppendMenuW(menu, MF_STRING, ID_CORNER + 1, L"Top right");
    AppendMenuW(menu, MF_STRING, ID_CORNER + 2, L"Bottom left"); AppendMenuW(menu, MF_STRING, ID_CORNER + 3, L"Bottom right");
    RECT rc; GetClientRect(hwnd, &rc);
    POINT p = { Px(hwnd, BtnX(rc.right / Scale(hwnd), BTN_CORNER)), Px(hwnd, HEADER_H) }; ClientToScreen(hwnd, &p);
    Popup(hwnd, menu, p.x, p.y);
}
static void Command(HWND hwnd, int id)
{
    switch (id) {
    case ID_SHOW: ShowPanel(hwnd, !IsWindowVisible(hwnd)); break;
    case ID_DARK: case ID_LIGHT: case ID_FOLLOW: g_set.theme = id - ID_DARK; ApplyTheme(); SettingsSave(); break;
    case ID_TOPMOST: SetTopmost(hwnd, !g_set.topmost); break;
    case ID_AUTOSTART: SetAutostart(!Autostart()); break;
    case ID_EXIT: DestroyWindow(hwnd); break;
    default: if (id >= ID_CORNER && id < ID_CORNER + 4) MoveToCorner(hwnd, (id - ID_CORNER) & 1, (id - ID_CORNER) >= 2);
    }
}
static void Button(HWND hwnd, int k)
{
    switch (k) {
    case BTN_FIT: DoFit(hwnd); break;
    case BTN_CORNER: CornerMenu(hwnd); break;
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
static void SetHot(HWND hwnd, int k)
{
    if (k == g_ui.hot) return;
    g_ui.hot = k; RECT rc; GetClientRect(hwnd, &rc); rc.bottom = Px(hwnd, HEADER_H); InvalidateRect(hwnd, &rc, FALSE);
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
    case WM_NCMOUSEMOVE: SetHot(hwnd, -1); break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* m = (MINMAXINFO*)lp; int h = Px(hwnd, PanelHeight(g_state));
        m->ptMinTrackSize.x = Px(hwnd, MinWidth()); m->ptMaxTrackSize.x = Px(hwnd, 1600); m->ptMinTrackSize.y = m->ptMaxTrackSize.y = h;
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        MetricsTick(g_state); SlowCopy(g_state); FitHeight(hwnd);
        if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);            // nascosto nel tray legge, così il minuto è pronto quando torna, ma non disegna
        if (g_seconds && ++g_ticks >= (unsigned)g_seconds) DestroyWindow(hwnd);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp)) { g_set.width = (int)(LOWORD(lp) / Scale(hwnd) + .5f); DrawResize(hwnd); InvalidateRect(hwnd, nullptr, FALSE); }
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
        SetHot(hwnd, ButtonAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_MOUSELEAVE: g_tracking = false; SetHot(hwnd, -1); return 0;
    case WM_LBUTTONDOWN: g_pressed = ButtonAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); if (g_pressed >= 0) SetCapture(hwnd); return 0;
    case WM_LBUTTONUP: {
        int k = ButtonAt(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), was = g_pressed;
        g_pressed = -1; if (GetCapture() == hwnd) ReleaseCapture();
        if (k >= 0 && k == was) Button(hwnd, k);
        return 0; }
    case WM_TRAY:                                                                   // NOTIFYICON_VERSION_4: l'evento è nella parola bassa, il punto in wParam
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: ShowPanel(hwnd, !IsWindowVisible(hwnd)); break;
        case WM_CONTEXTMENU: TrayMenu(hwnd, GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
        }
        return 0;
    case WM_COMMAND: Command(hwnd, LOWORD(wp)); return 0;
    case WM_DESTROY: TrayRemove(hwnd); SettingsSave(); SlowStop(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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
    int w = g_set.width, h = (int)(PanelHeight(g_state) + .5f);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return 2;
    SelectObject(dc, bmp);
    bool ok = DrawToDc(dc, w, h, g_state);
    GdiFlush();
    if (!ok) return 3;
    BITMAPFILEHEADER fh = {}; DWORD size = (DWORD)w * h * 4;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader); fh.bfSize = fh.bfOffBits + size;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 4;
    DWORD wr; WriteFile(f, &fh, sizeof(fh), &wr, nullptr); WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr); WriteFile(f, bits, size, &wr, nullptr);
    CloseHandle(f); return 0;
}

static void DumpLine(HANDLE f, const Sb& b)
{
    char utf8[400]; int n = WideCharToMultiByte(CP_UTF8, 0, b.s, b.n, utf8, sizeof(utf8) - 2, nullptr, nullptr);
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
    CloseHandle(f); return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;
    g_nosave = Find(cmd, L"--nosave") != nullptr; g_noact = Find(cmd, L"--noactivate") != nullptr;
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
    ApplyTheme();
    if (const wchar_t* p = Find(cmd, L"--shot")) return Shot(p);
    if (const wchar_t* p = Find(cmd, L"--dump")) return Dump(p);

    MetricsInit(g_state); SlowStart();
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
    TrayAdd(hwnd);
    if (g_set.visible) {
        ShowWindow(hwnd, g_noact ? SW_SHOWNOACTIVATE : SW_SHOW);
        DrawPaint(hwnd, g_state, g_ui); ValidateRect(hwnd, nullptr);                // il primo fotogramma subito, senza aspettare WM_PAINT
    }
    { char line[96]; wsprintfA(line, "start first_frame_ms=%d pid=%u", MsSinceProcessStart(), GetCurrentProcessId()); LogLine(line); }
    SetTimer(hwnd, 1, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
