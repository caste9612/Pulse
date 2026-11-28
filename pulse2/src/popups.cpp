// Pulse 2: tooltip, menu e la finestra Aspetto. Tooltip e menu sono due finestrelle del pannello, disegnate con i suoi caratteri e
// il suo bordo (draw.cpp) al posto di quelle di Windows.
//
// Il tooltip non prende mai il fuoco e il mouse gli passa attraverso: è solo un'immagine vicino al puntatore. Il menu invece prende
// il primo piano, perché gli servono i tasti (frecce, Invio, Esc) e perché così Windows gli dice quando si clicca altrove; se il primo
// piano non gli viene dato, un controllo a tempo lo chiude lo stesso al primo clic fuori.
#include "app.h"
#include <windowsx.h>
#include <dwmapi.h>

static HINSTANCE g_inst;
static HWND g_panel, g_tip, g_menuWnd, g_apWnd;
static wchar_t g_tipText[1100];
static Menu g_menu; static bool g_menuTracking;

static RECT WorkAt(POINT p)
{
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi); return mi.rcWork;
}

// ---- tooltip ----
static LRESULT CALLBACK TipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
        TipPaint(ps.hdc, rc.right, rc.bottom, (float)GetDpiForWindow(g_panel), g_tipText);
        EndPaint(hwnd, &ps);
        return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// La misura è quella del testo, ai DPI del pannello. Appare sotto il puntatore, o sopra se sotto finirebbe fuori dallo schermo
void TipShow(const wchar_t* text, POINT at)
{
    if (!g_tip || !text[0]) { TipHide(); return; }
    const bool shown = IsWindowVisible(g_tip) != 0;
    if (shown && !lstrcmpW(text, g_tipText)) return;
    lstrcpynW(g_tipText, text, 1100);
    float w = 0, h = 0; TipSize(g_tipText, w, h);
    if (w <= 0) { TipHide(); return; }
    const float sc = GetDpiForWindow(g_panel) / 96.f; const int pw = (int)ceilf(w * sc), ph = (int)ceilf(h * sc);
    if (shown) {                                                                     // i valori sono cambiati mentre era aperto: stesso posto, misura nuova
        SetWindowPos(g_tip, nullptr, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(g_tip, nullptr, FALSE);
        return;
    }
    const RECT work = WorkAt(at); int x = at.x, y = at.y + (int)(20 * sc);
    if (x + pw > work.right) x = work.right - pw;
    if (x < work.left) x = work.left;
    if (y + ph > work.bottom) y = at.y - ph - (int)(6 * sc);
    SetWindowPos(g_tip, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_tip, nullptr, FALSE);
}
void TipHide()
{
    if (g_tip && IsWindowVisible(g_tip)) ShowWindow(g_tip, SW_HIDE);
}

// ---- menu ----
// chiude il menu; con una voce scelta, il suo comando va al pannello
static void MenuDone(int id)
{
    if (!g_menuWnd) return;
    HWND w = g_menuWnd; g_menuWnd = nullptr;                                         // prima di distruggerla: perdendo il primo piano la finestra richiama qui
    DestroyWindow(w);
    if (id) PostMessageW(g_panel, WM_COMMAND, (WPARAM)id, 0);
    PostMessageW(g_panel, WM_MENU_CLOSED, 0, 0);
}
static void MenuHot(HWND hwnd, int i)
{
    if (i != g_menu.hot) { g_menu.hot = i; InvalidateRect(hwnd, nullptr, FALSE); }
}
// coi tasti: la voce successiva o precedente tra quelle che si scelgono, girando dall'ultima alla prima
static void MenuStep(HWND hwnd, int dir)
{
    int i = g_menu.hot;
    for (int k = 0; k < g_menu.n; k++) {
        i = i < 0 ? (dir > 0 ? 0 : g_menu.n - 1) : (i + dir + g_menu.n) % g_menu.n;
        if (g_menu.it[i].kind == MI_ITEM) { MenuHot(hwnd, i); return; }
    }
}
static int MenuItemUnder(HWND hwnd, LPARAM lp)
{
    return MenuItemAt(g_menu, GET_Y_LPARAM(lp) * 96.f / GetDpiForWindow(hwnd));
}
static LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
        MenuPaint(ps.hdc, rc.right, rc.bottom, (float)GetDpiForWindow(hwnd), g_menu);
        EndPaint(hwnd, &ps);
        return 0; }
    case WM_MOUSEMOVE:
        if (!g_menuTracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; g_menuTracking = TrackMouseEvent(&t) != 0; }
        MenuHot(hwnd, MenuItemUnder(hwnd, lp));
        return 0;
    case WM_MOUSELEAVE: g_menuTracking = false; MenuHot(hwnd, -1); return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {                                         // come nei menu di Windows, sceglie anche il tasto destro
        const int i = MenuItemUnder(hwnd, lp);
        if (i >= 0) MenuDone(g_menu.it[i].id);
        return 0; }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) MenuDone(0);
        else if (wp == VK_DOWN) MenuStep(hwnd, 1);
        else if (wp == VK_UP) MenuStep(hwnd, -1);
        else if (wp == VK_HOME) { g_menu.hot = -1; MenuStep(hwnd, 1); }
        else if (wp == VK_END) { g_menu.hot = -1; MenuStep(hwnd, -1); }
        else if ((wp == VK_RETURN || wp == VK_SPACE) && g_menu.hot >= 0) MenuDone(g_menu.it[g_menu.hot].id);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) MenuDone(0);                                  // si è cliccato altrove, o un'altra finestra è passata davanti
        return 0;
    case WM_TIMER:                                                                   // senza il primo piano Windows non avvisa: un tasto del mouse premuto fuori dal menu lo chiude
        if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) {
            POINT p; RECT rc; GetCursorPos(&p); GetWindowRect(hwnd, &rc);
            if (!PtInRect(&rc, p)) MenuDone(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// Si apre con l'angolo in alto a sinistra sul punto; se lì non ci sta, verso l'alto o verso sinistra, e mai sopra la barra delle
// applicazioni: quello dell'icona nel tray si apre sopra di lei
void MenuOpen(const Menu& m, POINT at)
{
    MenuDone(0); TipHide();
    g_menu = m; g_menu.hot = -1; g_menuTracking = false;
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Pulse2Menu", nullptr, WS_POPUP, at.x, at.y, 1, 1, g_panel, nullptr, g_inst, nullptr);
    if (!w) return;
    float fw = 0, fh = 0; MenuSize(g_menu, fw, fh);
    const float sc = GetDpiForWindow(w) / 96.f; const int pw = (int)ceilf(fw * sc), ph = (int)ceilf(fh * sc);
    const RECT work = WorkAt(at); int x = at.x, y = at.y;
    if (x + pw > work.right) x = at.x - pw;
    if (x < work.left) x = work.left;
    if (y + ph > work.bottom) y = (at.y < work.bottom ? at.y : work.bottom) - ph;
    if (y < work.top) y = work.top;
    g_menuWnd = w;
    SetWindowPos(w, HWND_TOPMOST, x, y, pw, ph, SWP_SHOWWINDOW);
    SetForegroundWindow(w);
    SetTimer(w, 1, 50, nullptr);
}
bool MenuIsOpen() { return g_menuWnd != nullptr; }

// ---- la finestra Aspetto: una finestra vera, con la barra del titolo di Windows; dentro, le scelte disegnate da Pulse. Un clic
// su una scelta la manda al pannello, che la applica e la salva; da lì si rilegge che cosa è in uso ----
static int g_apHot = -1; static bool g_apTracking;
static const int kApCommand[AP_GROUPS] = { ID_ACCENT, ID_DARK, ID_OPACITY, ID_DENSITY };
static int ApUnder(HWND hwnd, LPARAM lp)
{
    const float sc = 96.f / GetDpiForWindow(hwnd); return AppearanceAt(GET_X_LPARAM(lp) * sc, GET_Y_LPARAM(lp) * sc);
}
static void ApTitleBar(HWND hwnd)                                                     // scura col tema scuro
{
    BOOL dark = !g_theme->light; DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}
static LRESULT CALLBACK ApProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps); RECT rc; GetClientRect(hwnd, &rc);
        Appearance a; AppearanceGet(a); a.hot = g_apHot;
        AppearancePaint(ps.hdc, rc.right, rc.bottom, (float)GetDpiForWindow(hwnd), a);
        EndPaint(hwnd, &ps);
        return 0; }
    case WM_MOUSEMOVE: {
        if (!g_apTracking) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; g_apTracking = TrackMouseEvent(&t) != 0; }
        const int k = ApUnder(hwnd, lp);
        if (k != g_apHot) { g_apHot = k; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0; }
    case WM_MOUSELEAVE:
        g_apTracking = false;
        if (g_apHot != -1) { g_apHot = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const int k = ApUnder(hwnd, lp);
        if (k >= 0) { SendMessageW(g_panel, WM_COMMAND, (WPARAM)(kApCommand[k >> 4] + (k & 15)), 0); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0; }
    case WM_KEYDOWN: if (wp == VK_ESCAPE) DestroyWindow(hwnd); return 0;
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_DESTROY: g_apWnd = nullptr; g_apHot = -1; g_apTracking = false; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// al centro dello schermo su cui sta il pannello, e in primo piano come lui: mentre si provano le scelte deve restare visibile
void AppearanceOpen()
{
    if (g_apWnd) { SetForegroundWindow(g_apWnd); return; }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, ex = WS_EX_TOPMOST;
    RECT pr; GetWindowRect(g_panel, &pr); POINT c = { (pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2 }; const RECT work = WorkAt(c);
    HWND w = CreateWindowExW(ex, L"Pulse2Appearance", L"Appearance \x00B7 Pulse 2", style, work.left, work.top, 200, 200, nullptr, nullptr, g_inst, nullptr);
    if (!w) return;
    const UINT dpi = GetDpiForWindow(w); float fw = 0, fh = 0; AppearanceSize(fw, fh);
    RECT rc = { 0, 0, (int)ceilf(fw * dpi / 96), (int)ceilf(fh * dpi / 96) }; AdjustWindowRectExForDpi(&rc, style, FALSE, ex, dpi);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(w, nullptr, (work.left + work.right - ww) / 2, (work.top + work.bottom - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    ApTitleBar(w);
    g_apWnd = w; ShowWindow(w, SW_SHOW); SetForegroundWindow(w);
}

void PopupsInit(HINSTANCE inst, HWND panel)
{
    g_inst = inst; g_panel = panel;
    WNDCLASSEXW wc = { sizeof(wc) }; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = TipProc; wc.lpszClassName = L"Pulse2Tip"; RegisterClassExW(&wc);
    wc.lpfnWndProc = MenuProc; wc.lpszClassName = L"Pulse2Menu"; RegisterClassExW(&wc);
    wc.lpfnWndProc = ApProc; wc.lpszClassName = L"Pulse2Appearance"; wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); RegisterClassExW(&wc);
    // a strati e trasparente al mouse: i clic e i movimenti arrivano a quello che c'è sotto, di chiunque sia
    g_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"Pulse2Tip", nullptr, WS_POPUP,
                            0, 0, 1, 1, panel, nullptr, inst, nullptr);
    if (g_tip) SetLayeredWindowAttributes(g_tip, 0, 255, LWA_ALPHA);
}
void PopupsRedraw()
{
    if (g_tip) InvalidateRect(g_tip, nullptr, FALSE);
    if (g_menuWnd) InvalidateRect(g_menuWnd, nullptr, FALSE);
    if (g_apWnd) { ApTitleBar(g_apWnd); InvalidateRect(g_apWnd, nullptr, FALSE); }
}
