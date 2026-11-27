// Il pannello, disegnato con Direct2D e DirectWrite via software (non carica il driver della scheda video nel processo: è quello
// che pesava nella prova del 26/11/2026) su fondo opaco. Le misure sono in unità da 1/96 di pollice: il bersaglio porta i DPI della
// finestra. Ogni metrica ha la stessa colonna di testo, larga 168 da x 12, e il grafico da x 192 al bordo.
#include "app.h"
#include <d2d1_1.h>
#include <dwrite_3.h>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static ID2D1Factory1* g_f;
static IDWriteFactory5* g_dw;
static IDWriteInMemoryFontFileLoader* g_loader;
static IDWriteFontCollection1* g_col[NFILE];
static IDWriteTextFormat* g_fmt[F_COUNT];
static IDWriteInlineObject* g_dotsSign[F_COUNT];
static IDWriteTypography* g_tnum;
static ID2D1StrokeStyle* g_round;
static ID2D1RenderTarget* g_rt;          // il bersaglio in uso: la finestra o un DC in memoria
static ID2D1HwndRenderTarget* g_hrt;
static ID2D1SolidColorBrush* g_br;
static ID2D1BitmapBrush* g_dots;
static ID2D1LinearGradientBrush* g_grad[3];

static D2D1_COLOR_F Col(unsigned rgb, float a = 1) { return D2D1::ColorF(rgb, a); }

// i dati dei caratteri restano dove sono, nelle risorse dell'exe: questo oggetto dice a DirectWrite che non deve copiarli
struct StaticOwner : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { if (iid == __uuidof(IUnknown)) { *out = this; return S_OK; } *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};
static StaticOwner g_owner;

static bool InitFactories()
{
    if (g_f) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&g_f))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), (IUnknown**)&g_dw))) return false;
    if (FAILED(g_dw->CreateInMemoryFontFileLoader(&g_loader)) || FAILED(g_dw->RegisterFontFileLoader(g_loader))) return false;

    // una raccolta per file: nome della famiglia, peso e larghezza si leggono dal carattere stesso, senza indovinarli
    wchar_t fam[NFILE][64]; DWRITE_FONT_WEIGHT wt[NFILE]; DWRITE_FONT_STRETCH st[NFILE]; DWRITE_FONT_STYLE sy[NFILE];
    for (int i = 0; i < NFILE; i++) {
        IDWriteFontFile* file = nullptr; IDWriteFontSetBuilder1* b = nullptr; IDWriteFontSet* set = nullptr;
        IDWriteFontFamily* ff = nullptr; IDWriteLocalizedStrings* names = nullptr; IDWriteFont* font = nullptr;
        HRESULT hr = g_loader->CreateInMemoryFontFileReference(g_dw, g_fontData[i].p, g_fontData[i].n, &g_owner, &file);
        if (SUCCEEDED(hr)) hr = g_dw->CreateFontSetBuilder(&b);
        if (SUCCEEDED(hr)) hr = b->AddFontFile(file);
        if (SUCCEEDED(hr)) hr = b->CreateFontSet(&set);
        if (SUCCEEDED(hr)) hr = g_dw->CreateFontCollectionFromFontSet(set, &g_col[i]);
        if (SUCCEEDED(hr)) hr = g_col[i]->GetFontFamily(0, &ff);
        if (SUCCEEDED(hr)) hr = ff->GetFamilyNames(&names);
        if (SUCCEEDED(hr)) hr = names->GetString(0, fam[i], 64);
        if (SUCCEEDED(hr)) hr = ff->GetFont(0, &font);
        if (SUCCEEDED(hr)) { wt[i] = font->GetWeight(); st[i] = font->GetStretch(); sy[i] = font->GetStyle(); }
        Rel(font); Rel(names); Rel(ff); Rel(set); Rel(b); Rel(file);
        if (FAILED(hr)) return false;
    }
    for (int i = 0; i < F_COUNT; i++) {
        int k = kFonts[i].file;
        if (FAILED(g_dw->CreateTextFormat(fam[k], g_col[k], wt[k], sy[k], st[k], kFonts[i].px, L"en-us", &g_fmt[i]))) return false;
        g_fmt[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        g_dw->CreateEllipsisTrimmingSign(g_fmt[i], &g_dotsSign[i]);
    }
    if (SUCCEEDED(g_dw->CreateTypography(&g_tnum))) { DWRITE_FONT_FEATURE f = { DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 }; g_tnum->AddFontFeature(f); }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g_f->CreateStrokeStyle(sp, nullptr, 0, &g_round);
    return true;
}

// le risorse legate al bersaglio: il pennello, il letto puntinato sotto i grafici, le tre sfumature
static bool ResCreate(ID2D1RenderTarget* rt)
{
    Rel(g_br); Rel(g_dots); for (int i = 0; i < 3; i++) Rel(g_grad[i]);
    g_rt = rt;
    if (FAILED(rt->CreateSolidColorBrush(Col(C_TEXT), &g_br))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_SIZE_F sz = D2D1::SizeF(7, 7); D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ID2D1BitmapRenderTarget* crt = nullptr;
    if (SUCCEEDED(rt->CreateCompatibleRenderTarget(&sz, nullptr, &pf, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &crt))) {
        ID2D1SolidColorBrush* b = nullptr; ID2D1Bitmap* bm = nullptr;
        crt->BeginDraw(); crt->Clear(D2D1::ColorF(0, 0));
        if (SUCCEEDED(crt->CreateSolidColorBrush(Col(C_DOTS), &b))) { crt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(3.5f, 3.5f), .9f, .9f), b); b->Release(); }
        crt->EndDraw();
        if (SUCCEEDED(crt->GetBitmap(&bm))) { rt->CreateBitmapBrush(bm, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), &g_dots); bm->Release(); }
        crt->Release();
    }
    static const unsigned gc[3] = { C_CPU, C_RAM, C_GPU };
    for (int i = 0; i < 3; i++) {
        D2D1_GRADIENT_STOP stop[2] = { { 0, Col(gc[i], .28f) }, { 1, Col(gc[i], 0) } };
        ID2D1GradientStopCollection* sc = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stop, 2, &sc))) {
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), sc, &g_grad[i]); sc->Release();
        }
    }
    return true;
}

// ---- pezzi ----
static void R(float x, float y, float w, float h, unsigned col, float a = 1)
{
    if (w <= 0 || h <= 0) return;
    g_br->SetColor(Col(col, a)); g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_br);
}
// testo centrato in verticale sull'altezza delle maiuscole, con cifre tabulari; restituisce la larghezza. Con maxW il testo che non
// ci sta si accorcia con i puntini (i nomi che vengono dal PC); con draw = false si misura soltanto
static float Text(FontId f, float x, float ymid, const wchar_t* s, unsigned col, bool right = false, float maxW = 0, bool draw = true)
{
    UINT32 n = (UINT32)lstrlenW(s); IDWriteTextLayout* l = nullptr;
    if (!n || FAILED(g_dw->CreateTextLayout(s, n, g_fmt[f], maxW > 0 ? maxW : 4000, 100, &l))) return 0;
    DWRITE_TEXT_RANGE r = { 0, n };
    if (g_tnum) l->SetTypography(g_tnum, r);
    float track = kFonts[f].track * kFonts[f].px;
    if (track > 0) { IDWriteTextLayout1* l1 = nullptr; if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) { l1->SetCharacterSpacing(0, track, 0, r); l1->Release(); } }
    if (maxW > 0) { DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; l->SetTrimming(&t, g_dotsSign[f]); }
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m);
    float w = m.widthIncludingTrailingWhitespace - track;
    if (draw) {
        DWRITE_LINE_METRICS lm = {}; UINT32 c = 0; l->GetLineMetrics(&lm, 1, &c);
        float base = ymid + kFonts[f].cap * kFonts[f].px / 2;
        g_br->SetColor(Col(col));
        g_rt->DrawTextLayout(D2D1::Point2F(right ? x - w : x, base - lm.baseline), l, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    l->Release(); return w;
}
static float TextW(FontId f, const wchar_t* s) { return Text(f, 0, 0, s, 0, false, 0, false); }

static ID2D1PathGeometry* Lines(const float* xy, int n, bool closed)
{
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* k = nullptr;
    if (n < 2 || FAILED(g_f->CreatePathGeometry(&g))) return nullptr;
    if (FAILED(g->Open(&k))) { g->Release(); return nullptr; }
    k->BeginFigure(D2D1::Point2F(xy[0], xy[1]), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i < n; i++) k->AddLine(D2D1::Point2F(xy[2 * i], xy[2 * i + 1]));
    k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); k->Close(); k->Release();
    return g;
}
static void Stroke(const float* xy, int n, float ox, float oy, float width, unsigned col, float a = 1)
{
    float p[2 * (HIST + 2)]; if (n > HIST + 2) n = HIST + 2;
    for (int i = 0; i < n; i++) { p[2 * i] = ox + xy[2 * i]; p[2 * i + 1] = oy + xy[2 * i + 1]; }
    if (ID2D1PathGeometry* g = Lines(p, n, false)) { g_br->SetColor(Col(col, a)); g_rt->DrawGeometry(g, g_br, width, g_round); g->Release(); }
}
// freccia disegnata (i caratteri di Reticle non hanno le frecce)
static void Arrow(FontId f, float x, float ymid, bool down, unsigned col)
{
    float cap = kFonts[f].cap * kFonts[f].px, hw = cap * .32f, top = ymid - cap / 2, bot = ymid + cap / 2, cx = x + hw, st = kFonts[f].px / 11;
    float stem[4] = { cx, top, cx, bot }, head[6] = { cx - hw, down ? bot - hw : top + hw, cx, down ? bot : top, cx + hw, down ? bot - hw : top + hw };
    Stroke(stem, 2, 0, 0, st, col); Stroke(head, 3, 0, 0, st, col);
}
static void Bed(float x, float y, float w, float h)
{
    if (!g_dots) return;
    g_dots->SetTransform(D2D1::Matrix3x2F::Translation(x, y)); g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_dots);
}
static void Grid(float x, float y, float w, float h)                                // una riga ogni 10 secondi, alla stessa x in tutti i grafici
{
    const float xw = (w - 4) / (HIST - 1);
    for (int k = 1; k <= 5; k++) R(floorf(x + (HIST - 1 - 10 * k) * xw + .5f), y, 1, h, C_TEXT, .07f);
}
static ID2D1PathGeometry* Curve(const float* v, float x, float y, float w, float h, float max, bool closed)
{
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* k = nullptr;
    if (FAILED(g_f->CreatePathGeometry(&g))) return nullptr;
    if (FAILED(g->Open(&k))) { g->Release(); return nullptr; }
    const float top = y + 3, bot = y + h - 1.5f, xw = (w - 4) / (HIST - 1);
    auto X = [&](int i) { return x + i * xw; };
    auto Y = [&](float val) { return bot - Clampf(val / max, 0, 1) * (bot - top); };
    k->BeginFigure(D2D1::Point2F(X(0), Y(v[0])), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 0; i < HIST - 1; i++) {                                            // la stessa spline cardinale di Pulse 1.x
        float y0 = Y(v[i > 0 ? i - 1 : 0]), y1 = Y(v[i]), y2 = Y(v[i + 1]), y3 = Y(v[i + 2 < HIST ? i + 2 : HIST - 1]);
        k->AddBezier(D2D1::BezierSegment(D2D1::Point2F(X(i) + xw / 3, Clampf(y1 + (y2 - y0) / 6, top, bot)),
                                         D2D1::Point2F(X(i + 1) - xw / 3, Clampf(y2 - (y3 - y1) / 6, top, bot)), D2D1::Point2F(X(i + 1), y2)));
    }
    if (closed) { k->AddLine(D2D1::Point2F(X(HIST - 1), y + h)); k->AddLine(D2D1::Point2F(x, y + h)); }
    k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); k->Close(); k->Release();
    return g;
}
// grafico del carico, con la temperatura come linea sottile sulla stessa scala 0-100; la linea si interrompe dove la temperatura non c'era
static void Lane(float x, float y, float w, float h, const float* v, unsigned col, int gi, const float* temp, const wchar_t* label)
{
    Bed(x, y, w, h); Grid(x, y, w, h);
    if (g_grad[gi]) if (ID2D1PathGeometry* a = Curve(v, x, y, w, h, 100, true)) {
        g_grad[gi]->SetStartPoint(D2D1::Point2F(0, y)); g_grad[gi]->SetEndPoint(D2D1::Point2F(0, y + h)); g_rt->FillGeometry(a, g_grad[gi]); a->Release();
    }
    if (ID2D1PathGeometry* l = Curve(v, x, y, w, h, 100, false)) { g_br->SetColor(Col(col)); g_rt->DrawGeometry(l, g_br, 1.5f, g_round); l->Release(); }
    const float top = y + 3, bot = y + h - 1.5f, xw = (w - 4) / (HIST - 1);
    if (temp) {
        float p[2 * HIST]; int n = 0;
        for (int i = 0; i <= HIST; i++) {
            if (i < HIST && Has(temp[i])) { p[2 * n] = x + i * xw; p[2 * n + 1] = bot - Clampf(temp[i] / 100, 0, 1) * (bot - top); n++; continue; }
            if (n > 1) Stroke(p, n, 0, 0, 1, C_TEMP);
            n = 0;
        }
    }
    g_br->SetColor(Col(col)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + (HIST - 1) * xw, bot - Clampf(v[HIST - 1] / 100, 0, 1) * (bot - top)), 2.2f, 2.2f), g_br);
    if (label) Text(F_MONO9, x + 3, y + 6, label, C_MUTED);
}
// rete e disco a specchio: entrata sopra, uscita sotto, su una scala tonda scritta nell'angolo. Linee dritte: sono valori a scatti
static void Mirror(float x, float y, float w, float h, const float* a, const float* b, unsigned col)
{
    Bed(x, y, w, h); Grid(x, y, w, h);
    float ma = MaxOf(a), mb = MaxOf(b); Nice nc = NiceScale(ma > mb ? ma : mb);
    const float m = floorf(y + h / 2 + .5f), xw = (w - 4) / (HIST - 1);
    for (int half = 0; half < 2; half++) {
        const float* v = half ? b : a; const float dir = half ? 1.f : -1.f;
        float p[2 * (HIST + 2)];
        p[0] = x; p[1] = m;
        for (int i = 0; i < HIST; i++) { p[2 * (i + 1)] = x + i * xw; p[2 * (i + 1) + 1] = m + dir * (1 + Clampf(v[i] / nc.max, 0, 1) * (h / 2 - 4)); }
        p[2 * (HIST + 1)] = x + (HIST - 1) * xw; p[2 * (HIST + 1) + 1] = m;
        if (ID2D1PathGeometry* g = Lines(p, HIST + 2, true)) { g_br->SetColor(Col(col, half ? .14f : .30f)); g_rt->FillGeometry(g, g_br); g->Release(); }
        Stroke(p + 2, HIST, 0, 0, 1.2f, col, half ? .7f : 1);
    }
    R(x, m, w, 1, col, .4f);
    Text(F_MONO9, x + 3, y + 6, nc.label, C_MUTED);
}
// una barra per core logico: i thread dello stesso core fisico attaccati, uno spazio più largo tra un core e l'altro
static void CoreBars(float x, float y, float w, float h, const State& s)
{
    int n = s.ncore; if (n <= 0) return;
    const float gapIn = 1, gapOut = 3;
    float gaps = 0; for (int i = 1; i < n; i++) gaps += s.phys[i] == s.phys[i - 1] ? gapIn : gapOut;
    float bw = (w - gaps) / n, fx = 0; if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        float xx = floorf(x + fx + .5f), ww = floorf(x + fx + bw + .5f) - xx; if (ww < 1) ww = 1;
        float hh = floorf(h * Clampf(s.cores[i], 0, 1) + .5f);
        R(xx, y, ww, h, C_CPU, .13f); R(xx, y + h - hh, ww, hh, C_CPU);
        fx += bw + (i + 1 < n && s.phys[i + 1] == s.phys[i] ? gapIn : gapOut);
    }
}
// la memoria come in Gestione attività: in uso, modificata, in cache, libera, nel colore della RAM a forza decrescente
static void MemBar(float x, float y, float w, float h, const State& s)
{
    if (s.ramTotal <= 0) return;
    float part[4] = { s.ramInUse, s.ramModified, s.ramStandby, s.ramFree };
    if (!s.ramParts) { part[0] = s.ramUsed; part[1] = 0; part[2] = 0; part[3] = s.ramAvail; }
    static const float alpha[4] = { 1, .6f, .3f, .1f };
    float fx = 0, unit = (w - 3) / s.ramTotal;
    for (int i = 0; i < 4; i++) { float ww = part[i] * unit; if (part[i] > 0) R(floorf(x + fx + .5f), y, ww < 1 ? 1 : floorf(ww + .5f), h, C_RAM, alpha[i]); fx += ww + 1; }
}
// il livello di adesso dietro il testo: una velatura, piena solo sul bordo basso, con una tacca sul massimo dell'ultimo minuto.
// Nessuna riga verticale taglia le scritte
static void Level(float y, float h, float frac, float peak, unsigned col)
{
    frac = Clampf(frac, 0, 1); peak = Clampf(peak, 0, 1);
    const float w = 176 * frac;
    R(8, y, 176, h, col, .05f); R(8, y, w, h, col, .18f); R(8, y + h - 2, w, 2, col); R(floorf(8 + 176 * peak) - 1, y + h - 6, 2, 6, C_TEXT);
}
// rete e disco: due livelli, entrata e uscita, su scala logaritmica da 1 KB/s a 1 GB/s (il grafico accanto ha già quella lineare).
// In mezze righe da 16 le cifre arrivano quasi al bordo: la tacca del massimo è un trattino alto quanto il bordo, sotto il testo
static void LevelPair(float y, const float* ha, const float* hb, unsigned col)
{
    const float x0 = 42, w = 142; float fa = w * Log6(ha[HIST - 1]), fb = w * Log6(hb[HIST - 1]);
    R(x0, y, w, 33, col, .05f);
    R(x0, y, fa, 16, col, .18f); R(x0, y + 14, fa, 2, col); R(Clampf(floorf(x0 + w * Log6(MaxOf(ha))) - 4, x0, x0 + w - 4), y + 14, 4, 2, C_TEXT);
    R(x0, y + 17, fb, 16, col, .12f); R(x0, y + 31, fb, 2, col, .6f); R(Clampf(floorf(x0 + w * Log6(MaxOf(hb))) - 4, x0, x0 + w - 4), y + 31, 4, 2, C_TEXT);
}
static void Row3(float y, const wchar_t* lab, unsigned col, const wchar_t* big, const wchar_t* sec, unsigned secCol)
{
    Text(F_LABEL, 12, y + 16, lab, col);
    Text(F_BIG, 112, y + 16, big, C_TEXT, true);                                    // i valori finiscono tutti sulla stessa verticale
    if (sec) Text(F_C15, 180, y + 16, sec, secCol, true);                           // e così il valore secondario
}
static void RowPair(float y, const wchar_t* lab, unsigned col, const wchar_t* a, const wchar_t* b)
{
    Text(F_LABEL, 12, y + 16, lab, col);
    const float ya = y + 16 - 8.4f, yb = y + 16 + 8.4f;
    // le due velocità finiscono sulla verticale dei valori grandi; una più larga del posto (da 100 MB/s in su) sposta tutte e due
    // a destra quanto serve per non finire sulla freccia
    float wa = TextW(F_C15, a), wb = TextW(F_C15, b), xr = 60 + (wa > wb ? wa : wb); if (xr < 112) xr = 112;
    Arrow(F_C15, 46, ya, true, C_TEXT); Text(F_C15, xr, ya, a, C_TEXT, true);
    Arrow(F_C15, 46, yb, false, C_TEXT); Text(F_C15, xr, yb, b, C_TEXT, true);
}
// Top CPU o Top RAM: un programma per riga, col numero dei suoi processi; dietro la riga il peso come istogramma
enum { TOP_H = 11 + NTOP * 17 + 7 + 2 };
static void Top(float x, float y, float w, const wchar_t* legend, const Program* rows, int n, bool cpu)
{
    g_br->SetColor(Col(C_LINE)); g_rt->DrawRectangle(D2D1::RectF(x + .5f, y + .5f, x + w - .5f, y + TOP_H - .5f), g_br, 1);
    float lw = TextW(F_LABEL, legend);                                              // la legenda sta sulla cornice, come nei Panel di Reticle
    R(x + 8, y - 5, lw + 12, 11, C_BG);
    Text(F_LABEL, x + 14, y, legend, C_TEXT);
    static const unsigned slotCol[3] = { C_P0, C_P1, C_P2 };
    bool swatch = false; for (int i = 0; i < n; i++) if (rows[i].slot >= -1) swatch = true;
    float top = n ? (cpu ? rows[0].cpu : rows[0].ramMb) : 1; if (cpu && top < 4) top = 4; if (top <= 0) top = 1;   // per la CPU la scala arriva almeno al 4 %
    for (int i = 0; i < n; i++) {
        const Program& p = rows[i];
        float mid = y + 12 + i * 17 + 8.5f, tx = x + 10;
        R(x + 10, y + 12 + i * 17 + 1, floorf(Clampf((cpu ? p.cpu : p.ramMb) / top, 0, 1) * (w - 20) + .5f), 15, cpu ? C_CPU : C_RAM, .26f);
        if (swatch) { if (p.slot >= 0 && p.slot < 3) R(tx + 2, mid - 3, 6, 6, slotCol[p.slot]); tx += 12; }
        Sb v; if (cpu) v.f1(p.cpu).c(L'%'); else v.ram(p.ramMb);
        float vw = Text(F_MONO, x + w - 12, mid, v.s, C_TEXT2, true);
        Sb c; if (p.count > 1) c.c(L'\x00D7').i(p.count);
        float cw = c.n ? TextW(F_SMALL, c.s) + 4 : 0, room = x + w - 12 - vw - 8 - cw - (tx + 2);
        if (room < 12) room = 12;
        float nw = Text(F_TEXT, tx + 2, mid, p.name, C_TEXT, false, room);
        if (nw > room) nw = room;
        if (c.n) Text(F_SMALL, tx + 2 + nw + 4, mid, c.s, C_MUTED);
    }
}

static void Uptime(Sb& b, ULONGLONG sec)
{
    ULONGLONG d = sec / 86400, h = sec / 3600 % 24, m = sec / 60 % 60;
    b.t(L"uptime ");
    if (d) b.i((long long)d).t(L"d ").i((long long)h).t(L"h ").i((long long)m).c(L'm');
    else if (h) b.i((long long)h).t(L"h ").i((long long)m).c(L'm');
    else b.i((long long)m).c(L'm');
}

float PanelHeight(const State& s) { return 36 + 63 * 3 + 65 + 56 + (float)s.ndrive * 17 + 12 + TOP_H + 12; }

static void DrawPanel(const State& s, float W, float H)
{
    const float X = 12, GX = 192, GW = W - GX - 14, RW = W - 2 * X, LH = 32;
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(Col(C_BG));
    R(0, 0, W, 1, C_LINE); R(0, H - 1, W, 1, C_LINE); R(0, 0, 1, H, C_LINE); R(W - 1, 0, 1, H, C_LINE);

    // testata: marchio, nome, da quanto è acceso il PC, la stima del consumo; a destra i quattro bottoni
    Stroke(kLogoPts, 9, 12, 8, 1.6f, C_ACCENT);
    float hx = 36; hx += Text(F_NAME, hx, 16, L"PULSE", C_TEXT) + 14;
    { Sb b; Uptime(b, s.uptimeSec); hx += Text(F_TEXT, hx, 16, b.s, C_TEXT2) + 14; }
    {   // CPU + scheda video + circa 30 W per il resto; senza la lettura vera della CPU è una stima, e lo dice la tilde
        bool measured = Has(s.cpuWatt) && s.cpuWatt > 0;
        float cpuW = measured ? s.cpuWatt : (10 + s.cpu * 1.5f > 170 ? 170 : 10 + s.cpu * 1.5f);
        float total = cpuW + (s.gpuOk && Has(s.gpuWatt) ? s.gpuWatt : 0) + 30;
        Sb b; b.t(measured ? L"PC " : L"PC ~").r(total).t(L" W"); Text(F_TEXT, hx, 16, b.s, C_TEMP);
    }
    for (int k = 0; k < NICONSEG; k++) Stroke(kIconSegs[k].p, kIconSegs[k].n, W - 4 - 28 * 4 + kIconSegs[k].icon * 28 + 6, 8, 1.2f, C_TEXT2);

    float y = 36;
    // CPU
    Level(y, LH, s.cpu / 100, MaxOf(s.hCpu) / 100, C_CPU);
    { Sb big, sec; big.f1(s.cpu).c(L'%'); if (Has(s.cpuTemp)) sec.r(s.cpuTemp).t(L"\x00B0" L"C"); else sec.t(L"n/a");
      Row3(y, L"CPU", C_CPU, big.s, sec.s, C_TEMP); }
    Lane(GX, y, GW, LH, s.hCpu, C_CPU, 0, s.hCt, L"100");
    { Sb b; if (s.ncore) b.t(L"\x2248 ").f1(s.coresInUse).t(L" cores");
      if (s.cpuGhz > 0) b.sep().f2(s.cpuGhz).t(L" GHz");
      if (Has(s.cpuWatt) && s.cpuWatt > 0) b.sep().r(s.cpuWatt).t(L" W");
      Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180); }
    CoreBars(GX, y + 36, GW, 12, s);
    R(X, y + 55, RW, 1, C_LINESOFT); y += 63;
    // RAM
    Level(y, LH, s.ramPct / 100, MaxOf(s.hRam) / 100, C_RAM);
    { Sb big, sec; big.r(s.ramPct).c(L'%'); sec.f1(s.ramUsed).t(L" GB"); Row3(y, L"RAM", C_RAM, big.s, sec.s, C_TEXT2); }
    Lane(GX, y, GW, LH, s.hRam, C_RAM, 1, nullptr, nullptr);
    { Sb b; b.f1(s.ramAvail).t(L" GB available of ").f1(s.ramTotal).t(L" GB"); Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180); }
    MemBar(GX, y + 38, GW, 8, s);
    R(X, y + 55, RW, 1, C_LINESOFT); y += 63;
    // GPU: senza una scheda letta restano la sigla e il grafico vuoto
    if (s.gpuOk) {
        Level(y, LH, s.gpu / 100, MaxOf(s.hGpu) / 100, C_GPU);
        Sb big, sec; big.r(s.gpu).c(L'%'); if (Has(s.gpuTemp)) sec.r(s.gpuTemp).t(L"\x00B0" L"C");
        Row3(y, L"GPU", C_GPU, big.s, sec.n ? sec.s : nullptr, C_TEMP);
        Sb b; if (s.vramTotal > 0) b.t(L"VRAM ").f1(s.vramUsed).t(L" / ").f1(s.vramTotal).t(L" GB");
        if (Has(s.gpuWatt) && s.gpuWatt > 0) b.sep().r(s.gpuWatt).t(L" W");
        if (!b.n) b.t(s.gpuName);
        Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180);
        if (s.vramTotal > 0) { R(GX, y + 39, GW, 6, C_SURF2); R(GX, y + 39, GW * Clampf(s.vramUsed / s.vramTotal, 0, 1), 6, C_GPU); }
    } else Row3(y, L"GPU", C_GPU, L"\x2014", nullptr, C_TEMP);
    Lane(GX, y, GW, LH, s.hGpu, C_GPU, 2, s.hGt, nullptr);
    R(X, y + 55, RW, 1, C_LINESOFT); y += 63;
    // NET
    LevelPair(y, s.hDown, s.hUp, C_NET);
    { Sb a, b; a.rate(s.down); b.rate(s.up); RowPair(y, L"NET", C_NET, a.s, b.s); }
    Mirror(GX, y, GW, LH, s.hDown, s.hUp, C_NET);
    { Sb b; b.t(s.adapter); if (Has(s.pingMs)) b.sep().t(L"ping ").r(s.pingMs).t(L" ms"); Text(F_L2, X, y + 43, b.s, C_MUTED, false, 180); }
    { Sb b; if (s.localIp[0]) b.t(L"local ").t(s.localIp); if (s.publicIp[0]) b.sep().t(L"pub ").t(s.publicIp); Text(F_L2, GX, y + 43, b.s, C_MUTED, false, GW); }
    R(X, y + 57, RW, 1, C_LINESOFT); y += 65;
    // DSK, e sotto il suo grafico il righello del tempo, che vale per tutti e cinque
    LevelPair(y, s.hDr, s.hDw, C_DSK);
    { Sb a, b; a.rate(s.dr); b.rate(s.dw); RowPair(y, L"DSK", C_DSK, a.s, b.s); }
    Mirror(GX, y, GW, LH, s.hDr, s.hDw, C_DSK);
    { const float xw = (GW - 4) / (HIST - 1);
      for (int k = 5; k >= 1; k--) { Sb b; b.i(k * 10).t(L" s"); float w = TextW(F_MONO9, b.s); Text(F_MONO9, GX + (HIST - 1 - 10 * k) * xw - w / 2, y + 43, b.s, C_MUTED); }
      Text(F_MONO9, GX + GW - 4, y + 43, L"now", C_MUTED, true); }
    y += 56;
    // dischi: una riga per disco, il testo in una colonna larga quanto il più lungo, così le barre sono lunghe uguale
    Sb dv[MAXDRIVE]; float dw = 0;
    for (int i = 0; i < s.ndrive; i++) { dv[i].gb(s.drives[i].freeGb).t(L" free of ").gb(s.drives[i].totalGb); float w = TextW(F_MONO, dv[i].s); if (w > dw) dw = w; }
    const float bw = RW - 30 - dw - 8;
    for (int i = 0; i < s.ndrive; i++) {
        float mid = y + i * 17 + 8.5f, used = s.drives[i].totalGb > 0 ? 1 - s.drives[i].freeGb / s.drives[i].totalGb : 0;
        Text(F_TEXT, X, mid, s.drives[i].name, C_TEXT2);
        Text(F_MONO, X + RW, mid, dv[i].s, C_TEXT2, true);
        R(X + 30, floorf(mid - 2.5f), bw, 5, C_LINESOFT); R(X + 30, floorf(mid - 2.5f), bw * Clampf(used, 0, 1), 5, C_DSK);
    }
    y += (float)s.ndrive * 17 + 12;
    const float tw = floorf((RW - 12) / 2);
    Top(X, y, tw, L"TOP CPU", s.topCpu, s.nTopCpu, true);
    Top(X + RW - tw, y, tw, L"TOP RAM", s.topRam, s.nTopRam, false);
}

// ---- bersagli ----
bool DrawCreate(HWND hwnd)
{
    if (!InitFactories()) return false;
    RECT rc; GetClientRect(hwnd, &rc); float dpi = (float)GetDpiForWindow(hwnd);
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(g_f->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_hrt))) return false;
    return ResCreate(g_hrt);
}
void DrawResize(HWND hwnd)
{
    if (!g_hrt) return;
    RECT rc; GetClientRect(hwnd, &rc); float dpi = (float)GetDpiForWindow(hwnd);
    g_hrt->Resize(D2D1::SizeU(rc.right, rc.bottom)); g_hrt->SetDpi(dpi, dpi);
}
void DrawPaint(HWND hwnd, const State& s)
{
    if (!g_hrt) return;
    g_rt = g_hrt;
    D2D1_SIZE_F sz = g_hrt->GetSize();
    g_hrt->BeginDraw(); DrawPanel(s, sz.width, sz.height);
    if (g_hrt->EndDraw() == D2DERR_RECREATE_TARGET) { Rel(g_hrt); g_rt = nullptr; DrawCreate(hwnd); }   // il bersaglio si è perso: si rifà
}
bool DrawToDc(HDC memdc, int w, int h, const State& s)
{
    if (!InitFactories()) return false;
    ID2D1DCRenderTarget* dc = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
    if (FAILED(g_f->CreateDCRenderTarget(&rp, &dc))) return false;
    RECT rc = { 0, 0, w, h };
    if (FAILED(dc->BindDC(memdc, &rc)) || !ResCreate(dc)) return false;
    dc->BeginDraw(); DrawPanel(s, (float)w, (float)h);
    return SUCCEEDED(dc->EndDraw());
}
