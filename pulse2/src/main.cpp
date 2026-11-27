// Pulse 2: la finestra, il timer da un secondo, gli argomenti.
//
//   --pos <x> <y>       posizione della finestra
//   --width <n>         larghezza in unità (predefinita 514); l'altezza è sempre quella del contenuto
//   --seconds <n>       si chiude da solo dopo n secondi
//   --noactivate        non prende il fuoco quando si apre
//   --shot <file.bmp>   legge i dati per un secondo, disegna un fotogramma fuori schermo, lo salva ed esce (nessuna finestra)
//   --dump <file.txt>   legge i dati per un secondo e li scrive come testo, per confrontarli con i contatori di Windows
#include "app.h"

FontData g_fontData[NFILE];
static State g_state;
static int g_seconds, g_width = PANEL_W;
static unsigned g_ticks;

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

static int Px(HWND hwnd, float units) { return (int)(units * GetDpiForWindow(hwnd) / 96.f + .5f); }

// l'altezza segue il contenuto: cambia quando arriva l'elenco dei dischi, o un disco in più
static void FitHeight(HWND hwnd)
{
    RECT rc; GetWindowRect(hwnd, &rc);
    int h = Px(hwnd, PanelHeight(g_state)), w = Px(hwnd, (float)g_width);
    if (rc.bottom - rc.top != h || rc.right - rc.left != w) SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE: if (wp) return 0; break;                                    // tutta la finestra è area di disegno
    case WM_NCHITTEST: return HTCAPTION;                                            // per ora si trascina da ovunque; tasto destro: menu di sistema con Chiudi
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        MetricsTick(g_state); SlowCopy(g_state); FitHeight(hwnd); InvalidateRect(hwnd, nullptr, FALSE);
        if (g_seconds && ++g_ticks >= (unsigned)g_seconds) DestroyWindow(hwnd);
        return 0;
    case WM_SIZE: DrawResize(hwnd); return 0;
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        DrawResize(hwnd); FitHeight(hwnd); return 0; }
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        DrawPaint(hwnd, g_state);
        EndPaint(hwnd, &ps);
        return 0; }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(hwnd); return 0;
    case WM_DESTROY: SlowStop(); PostQuitMessage(0); return 0;
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
    int w = g_width, h = (int)(PanelHeight(g_state) + .5f);
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
    for (int i = 0; i < s.ndrive; i++) DumpLine(f, b.clear().t(L"drive ").t(s.drives[i].name).t(L" free_gb ").f2(s.drives[i].freeGb).t(L" total_gb ").f2(s.drives[i].totalGb));
    for (int i = 0; i < s.nTopCpu; i++) DumpLine(f, b.clear().t(L"top_cpu ").t(s.topCpu[i].name).t(L" x").i(s.topCpu[i].count).t(L" cpu ").f2(s.topCpu[i].cpu).t(L" pid ").i(s.topCpu[i].pid));
    for (int i = 0; i < s.nTopRam; i++) DumpLine(f, b.clear().t(L"top_ram ").t(s.topRam[i].name).t(L" x").i(s.topRam[i].count).t(L" mb ").r(s.topRam[i].ramMb).t(L" pid ").i(s.topRam[i].pid));
    CloseHandle(f); return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LoadFonts();
    if (const wchar_t* p = Find(cmd, L"--width")) { g_width = ReadInt(p); if (g_width < 420) g_width = 420; if (g_width > 1600) g_width = 1600; }
    if (const wchar_t* p = Find(cmd, L"--shot")) return Shot(p);
    if (const wchar_t* p = Find(cmd, L"--dump")) return Dump(p);

    int x = 120, y = 120;
    if (const wchar_t* p = Find(cmd, L"--pos")) { x = ReadInt(p); y = ReadInt(p); }
    if (const wchar_t* p = Find(cmd, L"--seconds")) g_seconds = ReadInt(p);
    bool noact = Find(cmd, L"--noactivate") != nullptr;

    MetricsInit(g_state); SlowStart();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"Pulse2";
    RegisterClassExW(&wc);
    // stile con cornice (angoli tondi e ombra li dà Windows); WM_NCCALCSIZE la toglie dal disegno
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (noact ? WS_EX_NOACTIVATE : 0), wc.lpszClassName, L"Pulse 2",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, g_width, (int)PanelHeight(g_state), nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    SetWindowPos(hwnd, nullptr, x, y, Px(hwnd, (float)g_width), Px(hwnd, PanelHeight(g_state)), SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!DrawCreate(hwnd)) { LogLine("drawing start FAILED"); return 4; }
    ShowWindow(hwnd, noact ? SW_SHOWNOACTIVATE : SW_SHOW);
    DrawPaint(hwnd, g_state); ValidateRect(hwnd, nullptr);                          // il primo fotogramma subito, senza aspettare WM_PAINT
    { char line[96]; wsprintfA(line, "start first_frame_ms=%d pid=%u", MsSinceProcessStart(), GetCurrentProcessId()); LogLine(line); }
    SetTimer(hwnd, 1, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
