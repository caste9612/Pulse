// Il pannello, disegnato con Direct2D e DirectWrite via software (non carica il driver della scheda video nel processo: è quello
// che pesava nella prova del 26/11/2026) su fondo opaco. Le misure sono in unità da 1/96 di pollice: il bersaglio porta i DPI della
// finestra. Ogni metrica ha la stessa colonna di testo, larga 168 da x 12, e il grafico da x 192 al margine destro, 12 come il sinistro.
//
// Il disegno è quasi tutto il costo di Pulse 2 (le letture valgono un decimo), perciò:
// - quello che non cambia da un secondo all'altro (fondo, letti puntinati, righe del tempo, divisori, cornici, legende, righello) si
//   disegna una volta in un'immagine, e a ogni fotogramma si copia quella;
// - i testi già composti si tengono: di solito a ogni secondo ne cambiano una quindicina su novanta;
// - l'antialiasing delle forme si accende solo per curve, linee e punti: i rettangoli stanno sui pixel, e l'area sotto una curva ha
//   il bordo coperto dalla sua linea.
#include "app.h"
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <string.h>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static ID2D1Factory1* g_f;
static IDWriteFactory5* g_dw;
static IDWriteInMemoryFontFileLoader* g_loader;
static IDWriteFontCollection1* g_col[NFILE];
static IDWriteTextFormat* g_fmt[F_COUNT];
static IDWriteInlineObject* g_dotsSign[F_COUNT];
static IDWriteTypography* g_tnum;
static ID2D1StrokeStyle* g_round;
static ID2D1RenderTarget* g_rt;          // il bersaglio in uso: la finestra, un DC in memoria, o l'immagine delle parti fisse
static ID2D1HwndRenderTarget* g_hrt;
static ID2D1SolidColorBrush* g_br;
static ID2D1BitmapBrush* g_dots;
static ID2D1LinearGradientBrush* g_grad[5];  // CPU, RAM, GPU: dall'alto in basso; rete e disco: dai due bordi verso l'asse
// le parti fisse, già disegnate: valgono per una larghezza, un'altezza e un tema
static ID2D1Bitmap* g_static; static float g_staticW, g_staticH; static const Theme* g_staticTheme;

// scuro: quello di sempre. Chiaro: i colori di Pulse 1.4 col tema chiaro di Reticle, presi dalla sua finestra. Non sono costanti
// per un colore solo: l'accento, che si sceglie dalla finestra Aspetto (DrawSetAccent)
Theme g_dark = { 0x121518, 0x191d21, 0x2c3338, 0x1d2226, 0x2b3237, 0xe9edee, 0xaab3b8, 0x6c7780, 0x3ddc84,
                      0x4cc2ff, 0xb987f7, 0x3ddc84, 0xf5c84b, 0xff5a52, 0xff8a1f, 0xff6eb4, 0xb8e55c, 0x8c9eff, .07f, false };
Theme g_light = { 0xe7e6e1, 0xd5d4cf, 0xbdbcb8, 0xd5d4cf, 0xcfcfca, 0x14161a, 0x50555d, 0x8a8f97, 0x008f67,
                       0x0a8fc7, 0x7c3aed, 0x1b9e5a, 0xc9a000, 0xd62828, 0xc2410c, 0xc2185b, 0x5b8c00, 0x3949ab, .10f, true };
const Theme* g_theme = &g_dark;
static int g_density;                    // 0 compatta, 1 comoda: più aria tra le metriche e righe più alte negli elenchi; il testo non cambia misura

static D2D1_COLOR_F Col(unsigned rgb, float a = 1) { return D2D1::ColorF(rgb, a); }
static void Smooth(bool on) { g_rt->SetAntialiasMode(on ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED); }

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

// le risorse legate al bersaglio: il pennello, il letto puntinato sotto i grafici, le cinque sfumature
static bool ResCreate(ID2D1RenderTarget* rt)
{
    Rel(g_br); Rel(g_dots); Rel(g_static); for (int i = 0; i < 5; i++) Rel(g_grad[i]);
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
    const unsigned gc[5] = { C_CPU, C_RAM, C_GPU, C_NET, C_DSK };
    for (int i = 0; i < 5; i++) {
        D2D1_GRADIENT_STOP stop[3] = { { 0, Col(gc[i], .28f) }, { 1, Col(gc[i], 0) }, { 1, Col(gc[i], .28f) } };
        if (i >= 3) stop[1].position = .5f;                                         // a specchio: la stessa forza ai due bordi, niente in mezzo
        ID2D1GradientStopCollection* sc = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stop, i >= 3 ? 3 : 2, &sc))) {
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), sc, &g_grad[i]); sc->Release();
        }
    }
    return true;
}

// ---- l'accento e la densità: quello che si sceglie dalla finestra Aspetto ----
static float Lin(float v) { v /= 255; return v <= .03928f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f); }
static float Luminance(unsigned rgb)
{
    return .2126f * Lin((float)(rgb >> 16 & 255)) + .7152f * Lin((float)(rgb >> 8 & 255)) + .0722f * Lin((float)(rgb & 255));
}
static float Contrast(unsigned a, unsigned b)
{
    const float x = Luminance(a), y = Luminance(b); return ((x > y ? x : y) + .05f) / ((x < y ? x : y) + .05f);
}
// la stessa tinta, più scura di `step` (luminosità HSL, da 0 a 1)
static unsigned Darker(unsigned rgb, float step)
{
    const float r = (rgb >> 16 & 255) / 255.f, g = (rgb >> 8 & 255) / 255.f, b = (rgb & 255) / 255.f;
    const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
    float l = (mx + mn) / 2, h = 0, s = 0;                                          // h in sesti di giro
    if (d > 0) {
        s = l > .5f ? d / (2 - mx - mn) : d / (mx + mn);
        h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    }
    l = Clampf(l - step, 0, 1);
    const float a = s * (l < 1 - l ? l : 1 - l); static const float n[3] = { 0, 8, 4 };
    unsigned out = 0;
    for (int i = 0; i < 3; i++) {
        const float k = fmodf(n[i] + h * 2, 12), t = Clampf(k - 3 < 9 - k ? k - 3 : 9 - k, -1, 1);
        out = out << 8 | (unsigned)(255 * Clampf(l - a * t, 0, 1) + .5f);
    }
    return out;
}
// Nel tema scuro l'accento si usa com'è; nel chiaro si scurisce a passi finché stacca abbastanza dal fondo (contrasto 3): la regola
// di Reticle. Il logo sta tra le parti fisse, che vanno rifatte
void DrawSetAccent(unsigned dark, unsigned lightBase)
{
    unsigned c = lightBase;
    for (int i = 0; i < 12 && Contrast(c, 0xf2f1ed) < 3; i++) c = Darker(c, .04f);
    g_dark.accent = dark; g_light.accent = c;
    Rel(g_static);
}
void DrawSetDensity(int density) { g_density = density ? 1 : 0; Rel(g_static); }

// ---- testi ----
// Un testo composto (carattere, stringa, larghezza massima) si tiene finché serve: comporlo costa più che disegnarlo. Le voci che
// nessuno chiede da più tempo lasciano il posto alle nuove
struct TextEntry { IDWriteTextLayout* l; unsigned hash, stamp; float w, baseline, maxW; unsigned char f, n; wchar_t s[62]; };
static TextEntry g_tc[192]; static unsigned g_stamp = 1;

static IDWriteTextLayout* Compose(FontId f, const wchar_t* s, UINT32 n, float maxW, float& w, float& baseline)
{
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s, n, g_fmt[f], maxW > 0 ? maxW : 4000, 100, &l))) return nullptr;
    DWRITE_TEXT_RANGE r = { 0, n };
    if (g_tnum) l->SetTypography(g_tnum, r);
    float track = kFonts[f].track * kFonts[f].px;
    if (track > 0) { IDWriteTextLayout1* l1 = nullptr; if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) { l1->SetCharacterSpacing(0, track, 0, r); l1->Release(); } }
    if (maxW > 0) { DWRITE_TRIMMING t = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; l->SetTrimming(&t, g_dotsSign[f]); }
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m);
    DWRITE_LINE_METRICS lm = {}; UINT32 c = 0; l->GetLineMetrics(&lm, 1, &c);
    w = m.widthIncludingTrailingWhitespace - track; baseline = lm.baseline;
    return l;
}
// testo centrato in verticale sull'altezza delle maiuscole, con cifre tabulari; restituisce la larghezza. Con maxW il testo che non
// ci sta si accorcia con i puntini (i nomi che vengono dal PC); con draw = false si misura soltanto
static float Text(FontId f, float x, float ymid, const wchar_t* s, unsigned col, bool right = false, float maxW = 0, bool draw = true)
{
    UINT32 n = (UINT32)lstrlenW(s); if (!n) return 0;
    IDWriteTextLayout* l = nullptr; float w = 0, baseline = 0; bool cached = false;
    if (n <= 61) {
        unsigned h = 2166136261u ^ (unsigned)f; for (UINT32 i = 0; i < n; i++) h = (h ^ s[i]) * 16777619u;
        h ^= (unsigned)(int)(maxW * 4) * 2654435761u;
        TextEntry* slot = &g_tc[0];
        for (TextEntry& e : g_tc) {
            if (e.l && e.hash == h && e.f == f && e.n == n && e.maxW == maxW && !memcmp(e.s, s, n * sizeof(wchar_t))) { e.stamp = g_stamp; l = e.l; w = e.w; baseline = e.baseline; break; }
            if (!e.l) { if (slot->l) slot = &e; } else if (slot->l && e.stamp < slot->stamp) slot = &e;
        }
        if (!l) {
            l = Compose(f, s, n, maxW, w, baseline); if (!l) return 0;
            Rel(slot->l); slot->l = l; slot->hash = h; slot->stamp = g_stamp; slot->w = w; slot->baseline = baseline; slot->maxW = maxW; slot->f = (unsigned char)f; slot->n = (unsigned char)n;
            CopyMemory(slot->s, s, n * sizeof(wchar_t));
        }
        cached = true;
    } else { l = Compose(f, s, n, maxW, w, baseline); if (!l) return 0; }
    if (draw) {
        float base = ymid + kFonts[f].cap * kFonts[f].px / 2;
        g_br->SetColor(Col(col));
        g_rt->DrawTextLayout(D2D1::Point2F(right ? x - w : x, base - baseline), l, g_br, maxW > 0 ? D2D1_DRAW_TEXT_OPTIONS_CLIP : D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    if (!cached) l->Release();
    return w;
}
static float TextW(FontId f, const wchar_t* s) { return Text(f, 0, 0, s, 0, false, 0, false); }

// ---- pezzi ----
static void R(float x, float y, float w, float h, unsigned col, float a = 1)
{
    if (w <= 0 || h <= 0) return;
    g_br->SetColor(Col(col, a)); g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_br);
}
static void Frame(float x, float y, float w, float h, unsigned col)
{
    R(x, y, w, 1, col); R(x, y + h - 1, w, 1, col); R(x, y, 1, h, col); R(x + w - 1, y, 1, h, col);
}
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
    if (ID2D1PathGeometry* g = Lines(p, n, false)) { g_br->SetColor(Col(col, a)); Smooth(true); g_rt->DrawGeometry(g, g_br, width, g_round); Smooth(false); g->Release(); }
}
// freccia disegnata (i caratteri di Reticle non hanno le frecce)
static void Arrow(FontId f, float x, float ymid, bool down, unsigned col)
{
    float cap = kFonts[f].cap * kFonts[f].px, hw = cap * .32f, top = ymid - cap / 2, bot = ymid + cap / 2, cx = x + hw, st = kFonts[f].px / 11;
    float stem[4] = { cx, top, cx, bot }, head[6] = { cx - hw, down ? bot - hw : top + hw, cx, down ? bot : top, cx + hw, down ? bot - hw : top + hw };
    Stroke(stem, 2, 0, 0, st, col); Stroke(head, 3, 0, 0, st, col);
}
// il passo tra un secondo e il successivo in un grafico largo w: la curva va da un bordo all'altro del letto, lunga quanto le barre
// che ha sotto; il punto sull'ultimo valore sta a cavallo del bordo destro
static float StepX(float w) { return w / (HIST - 1); }
// il letto di un grafico: puntini, e una riga ogni 10 secondi alla stessa x in tutti i grafici. Fa parte delle parti fisse
static void Bed(float x, float y, float w, float h)
{
    if (g_dots) { g_dots->SetTransform(D2D1::Matrix3x2F::Translation(x, y)); g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_dots); }
    const float xw = StepX(w);
    for (int k = 1; k <= 5; k++) R(floorf(x + (HIST - 1 - 10 * k) * xw + .5f), y, 1, h, C_TEXT, g_theme->grid);
}
// ---- la curva dei grafici: una sola per tutti e cinque ----
// Passa per i valori, e tra un valore e il successivo non sale né scende oltre i due (pendenze di Steffen): un picco di rete o di
// disco non sembra più alto di quanto è stato, e un tratto fermo resta dritto. I valori arrivano già in y, un passo xw l'uno
// dall'altro a partire da x0
struct Shape {
    ID2D1PathGeometry* g; ID2D1GeometrySink* k;
    bool Open()
    {
        g = nullptr; k = nullptr;
        if (FAILED(g_f->CreatePathGeometry(&g))) return false;
        if (FAILED(g->Open(&k))) { g->Release(); g = nullptr; return false; }
        return true;
    }
    void Spline(const float* ys, int n, float x0, float xw)
    {
        float m0 = ys[1] - ys[0];
        for (int i = 0; i + 1 < n; i++) {
            float d = ys[i + 1] - ys[i], m1 = d;
            if (i + 2 < n) {
                float e = ys[i + 2] - ys[i + 1];
                if (d * e <= 0) m1 = 0;                                             // un massimo o un minimo: lì la curva è piatta
                else { float ad = fabsf(d), ae = fabsf(e), lim = 2 * (ad < ae ? ad : ae); m1 = (d + e) / 2; if (fabsf(m1) > lim) m1 = m1 < 0 ? -lim : lim; }
            }
            k->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x0 + i * xw + xw / 3, ys[i] + m0 / 3), D2D1::Point2F(x0 + (i + 1) * xw - xw / 3, ys[i + 1] - m1 / 3),
                                             D2D1::Point2F(x0 + (i + 1) * xw, ys[i + 1])));
            m0 = m1;
        }
    }
    void Line(const float* ys, int n, float x0, float xw)
    {
        if (n < 2) return;
        k->BeginFigure(D2D1::Point2F(x0, ys[0]), D2D1_FIGURE_BEGIN_HOLLOW); Spline(ys, n, x0, xw); k->EndFigure(D2D1_FIGURE_END_OPEN);
    }
    void Area(const float* ys, int n, float x0, float xw, float base)             // l'area tra la curva e la riga base
    {
        if (n < 2) return;
        k->BeginFigure(D2D1::Point2F(x0, ys[0]), D2D1_FIGURE_BEGIN_FILLED); Spline(ys, n, x0, xw);
        k->AddLine(D2D1::Point2F(x0 + (n - 1) * xw, base)); k->AddLine(D2D1::Point2F(x0, base)); k->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    ID2D1PathGeometry* Done() { k->Close(); k->Release(); k = nullptr; return g; }
};
// i tratti che i grafici hanno in comune: linea da 1,5 con le giunte tonde, sfumatura sotto, un punto sull'ultimo valore
static const float kLineW = 1.5f, kDotR = 2.2f, kPadTop = 3;
static const float kDriveFull = .8f;     // da qui in su la barra di un disco diventa rossa
static void Dot(float x, float y, unsigned col) { g_br->SetColor(Col(col)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), kDotR, kDotR), g_br); }

// grafico del carico, con la temperatura come linea sottile sulla stessa scala 0-100; la linea si interrompe dove la temperatura non c'era
static void Lane(float x, float y, float w, float h, const float* v, unsigned col, int gi, const float* temp)
{
    const float top = y + kPadTop, bot = y + h - 1.5f, xw = StepX(w);
    float ys[HIST]; for (int i = 0; i < HIST; i++) ys[i] = bot - Clampf(v[i] / 100, 0, 1) * (bot - top);
    Shape s;
    if (g_grad[gi] && s.Open()) {                                                   // l'area: il suo bordo sta sotto la linea, non serve ammorbidirlo
        s.Area(ys, HIST, x, xw, y + h); ID2D1PathGeometry* a = s.Done();
        g_grad[gi]->SetStartPoint(D2D1::Point2F(0, y)); g_grad[gi]->SetEndPoint(D2D1::Point2F(0, y + h)); g_rt->FillGeometry(a, g_grad[gi]); a->Release();
    }
    Smooth(true);
    if (s.Open()) { s.Line(ys, HIST, x, xw); ID2D1PathGeometry* l = s.Done(); g_br->SetColor(Col(col)); g_rt->DrawGeometry(l, g_br, kLineW, g_round); l->Release(); }
    Dot(x + (HIST - 1) * xw, ys[HIST - 1], col);
    if (temp) {                                                                     // la temperatura: la stessa curva, sottile, un tratto per ogni serie di letture riuscite
        float ts[HIST]; int n = 0; bool open = false;
        for (int i = 0; i <= HIST; i++) {
            if (i < HIST && Has(temp[i])) { ts[n++] = bot - Clampf(temp[i] / 100, 0, 1) * (bot - top); continue; }
            if (n > 1 && (open || (open = s.Open()))) s.Line(ts, n, x + (i - n) * xw, xw);
            n = 0;
        }
        if (open) { ID2D1PathGeometry* l = s.Done(); g_br->SetColor(Col(C_TEMP)); g_rt->DrawGeometry(l, g_br, 1, g_round); l->Release(); }
    }
    Smooth(false);
}
// la scala scritta nell'angolo di un grafico, sopra la curva: una velina del colore del fondo la tiene leggibile quando la curva
// le passa dietro
static void ScaleLabel(float x, float y, const wchar_t* s)
{
    R(x + 1, y + 1, floorf(TextW(F_MONO9, s) + 5), 11, C_BG, .6f);
    Text(F_MONO9, x + 3, y + 6, s, C_MUTED);
}
// rete e disco a specchio: entrata sopra l'asse, uscita sotto, su una scala tonda scritta nell'angolo. Stessa curva, stesso spessore,
// stessa sfumatura e stesso punto finale dei grafici del carico. Le due linee si disegnano in un colpo solo: dove non passa niente
// stanno tutte e due sull'asse e fanno una linea sola, come quella di un grafico fermo
static void Mirror(float x, float y, float w, float h, const float* a, const float* b, unsigned col, int gi)
{
    float ma = MaxOf(a), mb = MaxOf(b); Nice nc = NiceScale(ma > mb ? ma : mb);
    const float m = floorf(y + h / 2 + .5f), ax = m + .5f, xw = StepX(w), span = h / 2 - kPadTop;
    float up[HIST], dn[HIST];
    for (int i = 0; i < HIST; i++) { up[i] = ax - Clampf(a[i] / nc.max, 0, 1) * span; dn[i] = ax + Clampf(b[i] / nc.max, 0, 1) * span; }
    Shape s;
    if (g_grad[gi] && s.Open()) {                                                   // piena verso i bordi, niente sull'asse
        s.Area(up, HIST, x, xw, ax); s.Area(dn, HIST, x, xw, ax); ID2D1PathGeometry* g = s.Done();
        g_grad[gi]->SetStartPoint(D2D1::Point2F(0, ax - h / 2)); g_grad[gi]->SetEndPoint(D2D1::Point2F(0, ax + h / 2)); g_rt->FillGeometry(g, g_grad[gi]); g->Release();
    }
    R(x, m, w, 1, col, .3f);                                                        // l'asse: si vede solo dove le due linee se ne staccano
    Smooth(true);
    if (s.Open()) { s.Line(up, HIST, x, xw); s.Line(dn, HIST, x, xw); ID2D1PathGeometry* l = s.Done(); g_br->SetColor(Col(col)); g_rt->DrawGeometry(l, g_br, kLineW, g_round); l->Release(); }
    Dot(x + (HIST - 1) * xw, up[HIST - 1], col);
    if (dn[HIST - 1] - up[HIST - 1] > .5f) Dot(x + (HIST - 1) * xw, dn[HIST - 1], col);
    Smooth(false);
    ScaleLabel(x, y, nc.label);
}
// una barra per core logico: i thread dello stesso core fisico attaccati, uno spazio più largo tra un core e l'altro
static void CoreBars(float x, float y, float w, float h, const State& s)
{
    int n = s.ncore; if (n <= 0) return;
    const float gapIn = 1, gapOut = 3; const unsigned slotCol[3] = { C_P0, C_P1, C_P2 };
    float gaps = 0; for (int i = 1; i < n; i++) gaps += s.phys[i] == s.phys[i - 1] ? gapIn : gapOut;
    float bw = (w - gaps) / n, fx = 0; if (bw < 1) bw = 1;
    for (int i = 0; i < n; i++) {
        float xx = floorf(x + fx + .5f), ww = floorf(x + fx + bw + .5f) - xx; if (ww < 1) ww = 1;
        float hh = floorf(h * Clampf(s.cores[i], 0, 1) + .5f), base = y + h;
        R(xx, y, ww, h, C_CPU, .13f);
        // a strati: dal basso la parte di ciascun programma seguito nel suo colore, il resto nel colore della CPU
        for (int k = 0; k < 3; k++) {
            float ph = floorf(hh * Clampf(s.share[i][k], 0, 1) + .5f); if (ph > base - (y + h - hh)) ph = base - (y + h - hh);
            if (ph > 0) { R(xx, base - ph, ww, ph, slotCol[k]); base -= ph; }
        }
        R(xx, y + h - hh, ww, base - (y + h - hh), C_CPU);
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
// ---- il livello dietro il testo: lo stesso riquadro per tutte e cinque le righe ----
static const float kBoxX = 8, kBoxW = 176;
// il massimo dell'ultimo minuto: la riga del bordo che prosegue, più tenue, da adesso fin lì. Quando adesso è il massimo non c'è
// niente da aggiungere. `now` è la lunghezza della riga piena, `peak` il massimo da 0 a 1
static void Trail(float yEdge, float now, float peak, unsigned col, float a)
{
    const float pw = floorf(kBoxW * Clampf(peak, 0, 1) + .5f);
    if (pw > now) R(kBoxX + now, yEdge, pw - now, 2, col, a * .42f);
}
// il livello di adesso: una velatura, piena solo sul bordo basso. Nessuna riga verticale taglia le scritte
static void Level(float y, float h, float frac, float peak, unsigned col)
{
    const float w = floorf(kBoxW * Clampf(frac, 0, 1) + .5f);
    R(kBoxX, y, kBoxW, h, col, .05f); R(kBoxX, y, w, h, col, .18f); R(kBoxX, y + h - 2, w, 2, col); Trail(y + h - 2, w, peak, col, 1);
}
// rete e disco: due livelli, entrata e uscita, su scala logaritmica da 1 KB/s a 1 GB/s (il grafico accanto ha già quella lineare).
// La sigla sta dentro il riquadro, a metà altezza: la riga dell'entrata va sul bordo alto e quella dell'uscita sul basso, così
// in mezzo non passa niente
static void LevelPair(float y, const float* ha, const float* hb, unsigned col)
{
    const float fa = floorf(kBoxW * Log6(ha[HIST - 1]) + .5f), fb = floorf(kBoxW * Log6(hb[HIST - 1]) + .5f);
    R(kBoxX, y, kBoxW, 33, col, .05f);
    R(kBoxX, y, fa, 16, col, .18f); R(kBoxX, y, fa, 2, col); Trail(y, fa, Log6(MaxOf(ha)), col, 1);
    R(kBoxX, y + 17, fb, 16, col, .12f); R(kBoxX, y + 31, fb, 2, col, .6f); Trail(y + 31, fb, Log6(MaxOf(hb)), col, .6f);
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
    const float ya = y + 16.5f - 7.75f, yb = y + 16.5f + 7.75f;                     // tra le due righe di bordo, senza toccarle
    // le due velocità finiscono sulla verticale dei valori grandi; una più larga del posto (da 100 MB/s in su) sposta tutte e due
    // a destra quanto serve per non finire sulla freccia
    float wa = TextW(F_C15, a), wb = TextW(F_C15, b), xr = 60 + (wa > wb ? wa : wb); if (xr < 112) xr = 112;
    Arrow(F_C15, 46, ya, true, C_TEXT); Text(F_C15, xr, ya, a, C_TEXT, true);
    Arrow(F_C15, 46, yb, false, C_TEXT); Text(F_C15, xr, yb, b, C_TEXT, true);
}
// Top CPU o Top RAM: un programma per riga, col numero dei suoi processi; dietro la riga il peso come istogramma. La cornice e la
// legenda stanno tra le parti fisse (LegendFrame). rh è l'altezza di una riga
static void LegendFrame(float x, float y, float w, float h, const wchar_t* legend)
{
    Frame(x, y, w, h, C_LINE);
    float lw = TextW(F_LABEL, legend);                                              // la legenda sta sulla cornice, come nei Panel di Reticle
    R(x + 8, y - 5, lw + 12, 11, C_BG);
    Text(F_LABEL, x + 14, y, legend, C_TEXT);
}
static void TopRows(float x, float y, float w, float rh, const Program* rows, int n, bool cpu, int hot)
{
    const unsigned slotCol[3] = { C_P0, C_P1, C_P2 };
    bool swatch = false; for (int i = 0; i < n; i++) if (rows[i].slot >= -1) swatch = true;
    float top = n ? (cpu ? rows[0].cpu : rows[0].ramMb) : 1; if (cpu && top < 4) top = 4; if (top <= 0) top = 1;   // per la CPU la scala arriva almeno al 4 %
    for (int i = 0; i < n; i++) {
        const Program& p = rows[i];
        float ry = y + 12 + i * rh, mid = ry + rh / 2, tx = x + 10;
        if (i == hot) R(x + 4, ry, w - 8, rh, C_TEXT, .07f);                         // la riga sotto il mouse: ha un tooltip e, col tasto destro, un menu
        R(x + 10, ry + 1, floorf(Clampf((cpu ? p.cpu : p.ramMb) / top, 0, 1) * (w - 20) + .5f), rh - 2, cpu ? C_CPU : C_RAM, .26f);
        if (swatch) { if (p.slot >= 0 && p.slot < 3) R(tx + 2, mid - 3, 6, 6, slotCol[p.slot]); tx += 12; }
        Sb v; if (cpu) v.f1(p.cpu).c(L'%'); else v.ram(p.ramMb);
        float vw = Text(F_MONO, x + w - 12, mid, v.s, C_TEXT2, true);
        Sb c; if (p.count > 1) c.c(L'\x00D7').i(p.count);
        float cw = c.n ? TextW(F_SMALL, c.s) + 4 : 0, room = floorf(x + w - 12 - vw - 8 - cw - (tx + 2));
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

// ---- le misure del pannello: una sola definizione per disegno, parti fisse, zone e larghezza minima ----
// air: l'aria in più tra una metrica e la successiva; rowH: l'altezza di una riga negli elenchi (dischi, programmi); topH: quella
// del riquadro di Top CPU e Top RAM. Con la densità comoda crescono, il testo no
struct Layout { float X, GX, GW, RW, yCpu, yRam, yGpu, yNet, yDsk, yDrives, yTop, tw, air, rowH, topH; };
static Layout Lay(float W, int ndrive)
{
    Layout l; l.X = 12; l.GX = 192; l.GW = W - l.GX - 12; l.RW = W - 24;
    l.air = g_density ? 6.f : 0.f; l.rowH = g_density ? 20.f : 17.f; l.topH = 11 + NTOP * l.rowH + 7 + 2;
    l.yCpu = 36 + l.air / 2; l.yRam = l.yCpu + 63 + l.air; l.yGpu = l.yRam + 63 + l.air; l.yNet = l.yGpu + 63 + l.air; l.yDsk = l.yNet + 65 + l.air;
    l.yDrives = l.yDsk + 56 + l.air; l.yTop = l.yDrives + (float)ndrive * l.rowH + 12 + l.air; l.tw = floorf((l.RW - 12) / 2);
    return l;
}
float PanelHeight(const State& s) { const Layout l = Lay(PANEL_W, s.ndrive); return l.yTop + l.topH + 12; }

// la larghezza più stretta in cui niente è tagliato, con ogni testo che cresce al suo più lungo (99 giorni di uptime, 999 W, due
// indirizzi pieni): adattare a traffico basso o appena avviati non taglia niente dopo. La colonna del testo ci sta per costruzione
float PanelFitWidth(const State& s)
{
    if (!InitFactories()) return PANEL_W;
    float header = 36 + TextW(F_NAME, L"PULSE") + 14 + TextW(F_TEXT, L"uptime 99d 23h 59m") + 14 + TextW(F_TEXT, L"PC ~999 W") + 12 + BTN_W * NBTN + 4;
    float addr = TextW(F_L2, L"local 255.255.255.255 \x00B7 pub 255.255.255.255"), graph = 192 + (addr > 96 ? addr : 96) + 12;
    float dw = 0; for (int i = 0; i < s.ndrive; i++) { Sb b; b.gb(s.drives[i].freeGb).t(L" free of ").gb(s.drives[i].totalGb); float w = TextW(F_MONO, b.s); if (w > dw) dw = w; }
    float drives = 12 + 30 + 60 + 8 + dw + 12, tops = 12 + 180 + 12 + 180 + 12;
    float w = header; if (graph > w) w = graph; if (drives > w) w = drives; if (tops > w) w = tops;
    return ceilf(w);
}

// le zone con un tooltip o un menu
int PanelRegions(const State& s, float W, Region* out, int cap)
{
    if (!InitFactories()) return 0;
    const Layout l = Lay(W, s.ndrive); int n = 0;
    auto add = [&](int id, float x, float y, float w, float h) { if (n < cap && w > 0 && h > 0) { out[n].id = id; out[n].x = x; out[n].y = y; out[n].w = w; out[n].h = h; n++; } };
    for (int k = 0; k < NBTN; k++) add(RG_BTN + k, BtnX(W, k), 2, BTN_W, HEADER_H - 4);
    { Sb b; Uptime(b, s.uptimeSec); float px = 36 + TextW(F_NAME, L"PULSE") + 14 + TextW(F_TEXT, b.s) + 14; add(RG_POWER, px, 6, TextW(F_TEXT, L"PC ~999 W"), 20); }
    add(RG_CPU_TEMP, 124, l.yCpu, 60, 32); add(RG_CORES, l.X, l.yCpu + 34, 180, 20); add(RG_CORES, l.GX, l.yCpu + 34, l.GW, 16);
    add(RG_RAM, l.X, l.yRam + 34, 180, 20); add(RG_RAM, l.GX, l.yRam + 34, l.GW, 16);
    add(RG_GPU, 8, l.yGpu, 176, 54); add(RG_GPU, l.GX, l.yGpu + 34, l.GW, 16);
    for (int i = 0; i < NTOP; i++) { add(RG_TOPCPU + i, l.X + 1, l.yTop + 12 + i * l.rowH, l.tw - 2, l.rowH); add(RG_TOPRAM + i, l.X + l.RW - l.tw + 1, l.yTop + 12 + i * l.rowH, l.tw - 2, l.rowH); }
    return n;
}

// ---- le parti fisse: tutto quello che dipende solo da misura e tema ----
static void DrawStatic(float W, float H, int ndrive)
{
    const Layout l = Lay(W, ndrive);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity()); Smooth(false);
    g_rt->Clear(Col(C_BG));
    Frame(0, 0, W, H, C_LINE);
    Stroke(kLogoPts, 9, 12, 8, 1.6f, C_ACCENT);
    Text(F_NAME, 36, 16, L"PULSE", C_TEXT);
    const float lanes[5] = { l.yCpu, l.yRam, l.yGpu, l.yNet, l.yDsk };
    for (int i = 0; i < 5; i++) { Bed(l.GX, lanes[i], l.GW, 32); if (i < 4) R(l.X, lanes[i] + (i < 3 ? 55 : 57) + l.air / 2, l.RW, 1, C_LINESOFT); }
    // il righello del tempo, sotto l'ultimo grafico: vale per tutti e cinque
    const float xw = StepX(l.GW);
    for (int k = 5; k >= 1; k--) { Sb b; b.i(k * 10).t(L" s"); float w = TextW(F_MONO9, b.s); Text(F_MONO9, l.GX + (HIST - 1 - 10 * k) * xw - w / 2, l.yDsk + 43, b.s, C_MUTED); }
    Text(F_MONO9, l.GX + l.GW, l.yDsk + 43, L"now", C_MUTED, true);
    LegendFrame(l.X, l.yTop, l.tw, l.topH, L"TOP CPU"); LegendFrame(l.X + l.RW - l.tw, l.yTop, l.tw, l.topH, L"TOP RAM");
}
// l'immagine delle parti fisse per questo bersaglio; se non si riesce a farla, le parti fisse si disegnano a ogni fotogramma
static bool StaticReady(float W, float H, int ndrive)
{
    if (g_static && g_staticW == W && g_staticH == H && g_staticTheme == g_theme) return true;
    Rel(g_static);
    ID2D1BitmapRenderTarget* crt = nullptr; D2D1_SIZE_F sz = D2D1::SizeF(W, H);
    if (FAILED(g_rt->CreateCompatibleRenderTarget(&sz, nullptr, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &crt))) return false;
    ID2D1RenderTarget* target = g_rt; g_rt = crt;
    crt->BeginDraw(); crt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); DrawStatic(W, H, ndrive);
    bool ok = SUCCEEDED(crt->EndDraw()) && SUCCEEDED(crt->GetBitmap(&g_static));
    g_rt = target; crt->Release();
    if (ok) { g_staticW = W; g_staticH = H; g_staticTheme = g_theme; }
    return ok;
}

// la scelta dell'angolo: quattro frecce in una tessera sotto il bottone, disposte come gli angoli dello schermo che indicano
static void DrawPicker(float W, int hot)
{
    static const float arrow[4][10] = {                                             // una spezzata di tre punti (la punta) e un segmento (l'asta), in 16 x 16
        { 4.5f,10, 4.5f,4.5f, 10,4.5f,   4.5f,4.5f, 12.5f,12.5f }, { 6,4.5f, 11.5f,4.5f, 11.5f,10,   11.5f,4.5f, 3.5f,12.5f },
        { 4.5f,6, 4.5f,11.5f, 10,11.5f,  4.5f,11.5f, 12.5f,3.5f }, { 11.5f,6, 11.5f,11.5f, 6,11.5f,  11.5f,11.5f, 3.5f,3.5f } };
    const float x = PickerX(W), y = HEADER_H, side = 2 * (PICK_PAD + PICK_CELL);
    R(x, y, side, side, C_BG); Frame(x, y, side, side, C_LINE);
    for (int k = 0; k < 4; k++) {
        float cx, cy; PickerCell(W, k, cx, cy);
        if (k == hot) R(cx, cy, PICK_CELL, PICK_CELL, C_TEXT, .09f);
        Stroke(arrow[k], 3, cx + 6, cy + 6, 1.3f, k == hot ? C_TEXT : C_TEXT2); Stroke(arrow[k] + 6, 2, cx + 6, cy + 6, 1.3f, k == hot ? C_TEXT : C_TEXT2);
    }
}

// ---- la lettura di un secondo passato: col mouse su un grafico, una riga su tutti e cinque coi valori di quel secondo ----
int GraphSecondAt(const State& s, float W, float x, float y)
{
    const Layout l = Lay(W, s.ndrive); const float lanes[5] = { l.yCpu, l.yRam, l.yGpu, l.yNet, l.yDsk };
    if (x < l.GX - 3 || x >= l.GX + l.GW + 3) return -1;
    for (int k = 0; k < 5; k++) {
        if (y < lanes[k] || y >= lanes[k] + 32) continue;
        const int i = (int)floorf((x - l.GX) / StepX(l.GW) + .5f);
        return i < 0 ? 0 : i > HIST - 1 ? HIST - 1 : i;
    }
    return -1;
}
// una targhetta col valore, accanto alla riga; per rete e disco porta la freccia della colonna di sinistra (dir 1 giù, 2 su)
static float PlateW(const wchar_t* s, int dir) { return floorf(TextW(F_MONO9, s) + (dir ? 17 : 9)); }
static void Plate(float x, float y, const wchar_t* s, int dir)
{
    const float w = PlateW(s, dir);
    R(x, y, w, 13, C_BG, .92f); Frame(x, y, w, 13, C_LINE);
    if (dir) Arrow(F_MONO9, x + 4, y + 6.5f, dir == 1, C_TEXT2);
    Text(F_MONO9, x + (dir ? 12 : 4), y + 6.5f, s, C_TEXT);
}
// il punto di quel secondo sulla curva, staccato dalla curva da un anello del colore del fondo
static void Mark(float x, float y, unsigned col)
{
    g_br->SetColor(Col(C_BG)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 4, 4), g_br); Dot(x, y, col);
}
static void Cross(const State& s, const Layout& l, int i)
{
    const float xw = StepX(l.GW), cx = l.GX + i * xw, x = floorf(cx);
    const float lanes[5] = { l.yCpu, l.yRam, l.yGpu, l.yNet, l.yDsk };
    const float* load[3] = { s.hCpu, s.hRam, s.hGpu }; const float* temp[3] = { s.hCt, nullptr, s.hGt }; const unsigned lc[3] = { C_CPU, C_RAM, C_GPU };
    const float* flow[4] = { s.hDown, s.hUp, s.hDr, s.hDw }; const unsigned mc[2] = { C_NET, C_DSK };
    // prima i testi: le targhette stanno tutte dallo stesso lato della riga, quello dove ci sta la più larga
    Sb t[7]; float wide = 0;
    for (int k = 0; k < 3; k++) {
        if (k == 2 && !s.gpuOk) continue;
        t[k].r(load[k][i]).c(L'%'); if (temp[k] && Has(temp[k][i])) t[k].sep().r(temp[k][i]).t(L"\x00B0" L"C");
    }
    for (int k = 0; k < 4; k++) t[3 + k].rate(flow[k][i]);
    for (int k = 0; k < 7; k++) if (t[k].n) { const float w = PlateW(t[k].s, k < 3 ? 0 : 1); if (w > wide) wide = w; }
    const bool left = x + 6 + wide > l.GX + l.GW;
    auto px = [&](int k) { const float v = left ? x - 5 - PlateW(t[k].s, k < 3 ? 0 : 1) : x + 6; return v < l.GX ? l.GX : v; };

    for (int k = 0; k < 5; k++) R(x, lanes[k], 1, 32, C_TEXT, .5f);
    float frac[3], fu[2], fd[2], ax[2];
    Smooth(true);
    for (int k = 0; k < 3; k++) {
        const float top = lanes[k] + kPadTop, bot = lanes[k] + 32 - 1.5f;
        frac[k] = Clampf(load[k][i] / 100, 0, 1);
        if (t[k].n) Mark(cx, bot - frac[k] * (bot - top), lc[k]);
    }
    for (int k = 0; k < 2; k++) {                                                    // le stesse misure di Mirror
        const float ma = MaxOf(flow[2 * k]), mb = MaxOf(flow[2 * k + 1]), span = 16 - kPadTop; const Nice nc = NiceScale(ma > mb ? ma : mb);
        ax[k] = floorf(lanes[3 + k] + 16.5f) + .5f;
        fu[k] = Clampf(flow[2 * k][i] / nc.max, 0, 1) * span; fd[k] = Clampf(flow[2 * k + 1][i] / nc.max, 0, 1) * span;
        Mark(cx, ax[k] - fu[k], mc[k]); if (fu[k] + fd[k] > .5f) Mark(cx, ax[k] + fd[k], mc[k]);
    }
    Smooth(false);
    // carico e temperatura: la targhetta sta nella metà del grafico dove la curva non è; rete e disco: entrata sopra l'asse, uscita sotto
    for (int k = 0; k < 3; k++) if (t[k].n) Plate(px(k), frac[k] > .5f ? lanes[k] + 18 : lanes[k] + 1, t[k].s, 0);
    for (int k = 0; k < 2; k++) { Plate(px(3 + 2 * k), lanes[3 + k] + 1, t[3 + 2 * k].s, 1); Plate(px(4 + 2 * k), lanes[3 + k] + 18, t[4 + 2 * k].s, 2); }
    // sul righello: quanti secondi fa
    Sb ago; if (i == HIST - 1) ago.t(L"now"); else ago.i(HIST - 1 - i).t(L" s ago");
    const float w = floorf(TextW(F_MONO9, ago.s) + 9), tx = Clampf(floorf(cx - w / 2), l.GX, l.GX + l.GW - w);
    R(tx, l.yDsk + 37, w, 13, C_TEXT); Text(F_MONO9, tx + 4, l.yDsk + 43.5f, ago.s, C_BG);
}

static void DrawPanel(const State& s, const Ui& ui, float W, float H)
{
    const Layout l = Lay(W, s.ndrive); const float X = l.X, GX = l.GX, GW = l.GW, RW = l.RW, LH = 32;
    g_stamp++;
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity()); Smooth(false);
    ID2D1RenderTarget* target = g_rt;
    if (StaticReady(W, H, s.ndrive)) target->DrawBitmap(g_static, D2D1::RectF(0, 0, W, H), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    else DrawStatic(W, H, s.ndrive);

    // testata: da quanto è acceso il PC, la stima del consumo; a destra i quattro bottoni
    float hx = 36 + TextW(F_NAME, L"PULSE") + 14;
    { Sb b; Uptime(b, s.uptimeSec); hx += Text(F_TEXT, hx, 16, b.s, C_TEXT2) + 14; }
    {   // CPU + scheda video + circa 30 W per il resto; senza la lettura vera della CPU è una stima, e lo dice la tilde
        bool measured = Has(s.cpuWatt) && s.cpuWatt > 0;
        float cpuW = measured ? s.cpuWatt : (10 + s.cpu * 1.5f > 170 ? 170 : 10 + s.cpu * 1.5f);
        float total = cpuW + (s.gpuOk && Has(s.gpuWatt) ? s.gpuWatt : 0) + 30;
        Sb b; b.t(measured ? L"PC " : L"PC ~").r(total).t(L" W"); Text(F_TEXT, hx, 16, b.s, C_TEXT2);     // un consumo, non una temperatura: niente arancione
    }
    if (ui.hot >= 0 && ui.hot < NBTN) R(BtnX(W, ui.hot), 2, BTN_W, HEADER_H - 4, C_TEXT, .09f);      // il bottone sotto il mouse
    if (ui.picker) R(BtnX(W, BTN_CORNER), 2, BTN_W, HEADER_H - 4, C_TEXT, .09f);
    for (int k = 0; k < NICONSEG; k++) {                                              // il fermo è nel colore dell'accento quando il widget sta in primo piano
        int icon = kIconSegs[k].icon;
        Stroke(kIconSegs[k].p, kIconSegs[k].n, BtnX(W, icon) + 6, 8, 1.2f, icon == BTN_PIN && ui.pinned ? C_ACCENT : C_TEXT2);
    }

    float y = l.yCpu;
    // CPU
    Level(y, LH, s.cpu / 100, MaxOf(s.hCpu) / 100, C_CPU);
    { Sb big, sec; big.f1(s.cpu).c(L'%'); if (Has(s.cpuTemp)) sec.r(s.cpuTemp).t(L"\x00B0" L"C"); else sec.t(L"n/a");
      Row3(y, L"CPU", C_CPU, big.s, sec.s, Has(s.cpuTemp) ? C_TEMP : C_MUTED); }                 // un valore che manca non spicca quanto uno che c'è
    Lane(GX, y, GW, LH, s.hCpu, C_CPU, 0, s.hCt);
    ScaleLabel(GX, y, L"100");
    { Sb b; if (s.ncore) b.t(L"\x2248 ").f1(s.coresInUse).t(L" cores");
      if (s.cpuGhz > 0) b.sep().f2(s.cpuGhz).t(L" GHz");
      if (Has(s.cpuWatt) && s.cpuWatt > 0) b.sep().r(s.cpuWatt).t(L" W");
      Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180); }
    CoreBars(GX, y + 36, GW, 12, s);
    // RAM
    y = l.yRam;
    Level(y, LH, s.ramPct / 100, MaxOf(s.hRam) / 100, C_RAM);
    { Sb big, sec; big.r(s.ramPct).c(L'%'); sec.f1(s.ramUsed).t(L" GB"); Row3(y, L"RAM", C_RAM, big.s, sec.s, C_TEXT2); }
    Lane(GX, y, GW, LH, s.hRam, C_RAM, 1, nullptr);
    ScaleLabel(GX, y, L"100");
    { Sb b; b.f1(s.ramAvail).t(L" GB available of ").f1(s.ramTotal).t(L" GB"); Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180); }
    MemBar(GX, y + 38, GW, 8, s);
    // GPU: senza una scheda letta restano la sigla e il grafico vuoto
    y = l.yGpu;
    if (s.gpuOk) {
        Level(y, LH, s.gpu / 100, MaxOf(s.hGpu) / 100, C_GPU);
        Sb big, sec; big.r(s.gpu).c(L'%'); if (Has(s.gpuTemp)) sec.r(s.gpuTemp).t(L"\x00B0" L"C");
        Row3(y, L"GPU", C_GPU, big.s, sec.n ? sec.s : nullptr, C_TEMP);
        Sb b; if (s.vramTotal > 0) b.t(L"VRAM ").f1(s.vramUsed).t(L" / ").f1(s.vramTotal).t(L" GB");
        if (Has(s.gpuWatt) && s.gpuWatt > 0) b.sep().r(s.gpuWatt).t(L" W");
        if (!b.n) b.t(s.gpuName);
        Text(F_L2, X, y + 42, b.s, C_MUTED, false, 180);
        if (s.vramTotal > 0) {                                                      // come la barra della memoria: alta 8, la parte libera nello stesso colore, tenue
            const float used = floorf(GW * Clampf(s.vramUsed / s.vramTotal, 0, 1) + .5f);
            R(GX, y + 38, used, 8, C_GPU); R(GX + used + 1, y + 38, GW - used - 1, 8, C_GPU, .1f);
        }
    } else Row3(y, L"GPU", C_GPU, L"\x2014", nullptr, C_TEMP);
    Lane(GX, y, GW, LH, s.hGpu, C_GPU, 2, s.hGt);
    ScaleLabel(GX, y, L"100");
    // NET
    y = l.yNet;
    LevelPair(y, s.hDown, s.hUp, C_NET);
    { Sb a, b; a.rate(s.down); b.rate(s.up); RowPair(y, L"NET", C_NET, a.s, b.s); }
    Mirror(GX, y, GW, LH, s.hDown, s.hUp, C_NET, 3);
    { Sb b; b.t(s.adapter); if (Has(s.pingMs)) b.sep().t(L"ping ").r(s.pingMs).t(L" ms"); Text(F_L2, X, y + 43, b.s, C_MUTED, false, 180); }
    { Sb b; if (s.localIp[0]) b.t(L"local ").t(s.localIp); if (s.publicIp[0]) b.sep().t(L"pub ").t(s.publicIp); Text(F_L2, GX, y + 43, b.s, C_MUTED, false, GW); }
    // DSK
    y = l.yDsk;
    LevelPair(y, s.hDr, s.hDw, C_DSK);
    { Sb a, b; a.rate(s.dr); b.rate(s.dw); RowPair(y, L"DSK", C_DSK, a.s, b.s); }
    Mirror(GX, y, GW, LH, s.hDr, s.hDw, C_DSK, 4);
    // dischi: una riga per disco, il testo in una colonna larga quanto il più lungo, così le barre sono lunghe uguale
    y = l.yDrives;
    Sb dv[MAXDRIVE]; float dw = 0;
    for (int i = 0; i < s.ndrive; i++) { dv[i].gb(s.drives[i].freeGb).t(L" free of ").gb(s.drives[i].totalGb); float w = TextW(F_MONO, dv[i].s); if (w > dw) dw = w; }
    const float bw = floorf(RW - 30 - dw - 8);
    for (int i = 0; i < s.ndrive; i++) {
        float mid = y + i * l.rowH + l.rowH / 2, used = s.drives[i].totalGb > 0 ? 1 - s.drives[i].freeGb / s.drives[i].totalGb : 0;
        Text(F_TEXT, X, mid, s.drives[i].name, C_TEXT2);
        Text(F_MONO, X + RW, mid, dv[i].s, C_TEXT2, true);
        // grigia finché c'è posto, nel rosso del disco da 80 % pieno in su: il rosso compare solo quando serve
        R(X + 30, floorf(mid - 2.5f), bw, 5, C_LINESOFT); R(X + 30, floorf(mid - 2.5f), floorf(bw * Clampf(used, 0, 1) + .5f), 5, used < kDriveFull ? C_MUTED : C_DSK);
    }
    TopRows(X, l.yTop, l.tw, l.rowH, s.topCpu, s.nTopCpu, true, ui.hotRegion >= RG_TOPCPU && ui.hotRegion < RG_TOPCPU + NTOP ? ui.hotRegion - RG_TOPCPU : -1);
    TopRows(X + RW - l.tw, l.yTop, l.tw, l.rowH, s.topRam, s.nTopRam, false, ui.hotRegion >= RG_TOPRAM && ui.hotRegion < RG_TOPRAM + NTOP ? ui.hotRegion - RG_TOPRAM : -1);
    if (ui.cross > 0 && ui.cross <= HIST) Cross(s, l, ui.cross - 1);
    if (ui.picker) DrawPicker(W, ui.hotArrow);
}

// ---- tooltip e menu: finestrelle a parte (popups.cpp), disegnate con gli stessi pezzi ----
// Sono piccole e di rado aperte: un solo bersaglio in memoria, legato ogni volta al DC di chi va disegnata, col suo pennello. Mentre
// si disegna lì, il bersaglio e il pennello del pannello restano da parte
static ID2D1DCRenderTarget* g_pop; static ID2D1SolidColorBrush* g_popBr;
static ID2D1RenderTarget* g_backRt; static ID2D1SolidColorBrush* g_backBr;
static bool PopBegin(HDC dc, int w, int h, float dpi)
{
    if (!InitFactories()) return false;
    if (!g_pop) {
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(g_f->CreateDCRenderTarget(&rp, &g_pop))) return false;
        if (FAILED(g_pop->CreateSolidColorBrush(Col(C_TEXT), &g_popBr))) { Rel(g_pop); return false; }
        g_pop->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    RECT rc = { 0, 0, w, h };
    if (FAILED(g_pop->BindDC(dc, &rc))) return false;
    g_pop->SetDpi(dpi, dpi);
    g_backRt = g_rt; g_backBr = g_br; g_rt = g_pop; g_br = g_popBr;
    g_pop->BeginDraw(); g_pop->SetTransform(D2D1::Matrix3x2F::Identity()); Smooth(false);
    return true;
}
static void PopEnd()
{
    if (g_pop->EndDraw() == D2DERR_RECREATE_TARGET) { Rel(g_popBr); Rel(g_pop); }   // si rifà alla prossima
    g_rt = g_backRt; g_br = g_backBr;
}

// un tooltip: righe da 17, a capo da solo oltre la larghezza massima; il fondo dei riquadri e il bordo del pannello
static const float kTipMaxW = 420, kTipPadX = 10, kTipPadY = 6, kTipLine = 17;
static IDWriteTextLayout* TipLayout(const wchar_t* text, float& w, float& h)
{
    IDWriteTextLayout* l = nullptr; const UINT32 n = (UINT32)lstrlenW(text);
    if (!n || !InitFactories() || FAILED(g_dw->CreateTextLayout(text, n, g_fmt[F_TEXT], kTipMaxW, 2000, &l))) return nullptr;
    l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kTipLine, 13);
    DWRITE_TEXT_RANGE r = { 0, n }; if (g_tnum) l->SetTypography(g_tnum, r);
    DWRITE_TEXT_METRICS m = {}; l->GetMetrics(&m);
    w = m.widthIncludingTrailingWhitespace; h = m.height;
    return l;
}
void TipSize(const wchar_t* text, float& w, float& h)
{
    float tw = 0, th = 0; w = h = 0;
    if (IDWriteTextLayout* l = TipLayout(text, tw, th)) { l->Release(); w = ceilf(tw) + 2 * kTipPadX; h = ceilf(th) + 2 * kTipPadY; }
}
void TipPaint(HDC dc, int wpx, int hpx, float dpi, const wchar_t* text)
{
    if (!PopBegin(dc, wpx, hpx, dpi)) return;
    float tw = 0, th = 0;
    g_rt->Clear(Col(C_SURF2)); Frame(0, 0, wpx * 96 / dpi, hpx * 96 / dpi, C_LINE);
    if (IDWriteTextLayout* l = TipLayout(text, tw, th)) { g_br->SetColor(Col(C_TEXT)); g_rt->DrawTextLayout(D2D1::Point2F(kTipPadX, kTipPadY), l, g_br); l->Release(); }
    PopEnd();
}

// un menu: voci alte 24, la voce sotto il mouse con la stessa velatura dei bottoni della testata, un segno di spunta nel colore
// dell'accento davanti alle voci accese. La colonna delle spunte c'è solo nei menu che ne hanno
static const float kMenuPad = 4, kMenuItemH = 24, kMenuSepH = 9, kMenuHeadH = 20;
static float MenuRowH(const MenuItem& it) { return it.kind == MI_SEP ? kMenuSepH : it.kind == MI_HEAD ? kMenuHeadH : kMenuItemH; }
static float MenuInset(const Menu& m)
{
    for (int i = 0; i < m.n; i++) if (m.it[i].checked) return 30;
    return 14;
}
void MenuSize(const Menu& m, float& w, float& h)
{
    w = 160; h = 2 * kMenuPad;
    if (!InitFactories()) return;
    const float inset = MenuInset(m);
    for (int i = 0; i < m.n; i++) {
        const MenuItem& it = m.it[i]; h += MenuRowH(it);
        if (it.kind == MI_SEP) continue;
        const float tw = ceilf(TextW(it.kind == MI_HEAD ? F_LABEL : F_TEXT, it.text)) + inset + 18;
        if (tw > w) w = tw;
    }
}
int MenuItemAt(const Menu& m, float y)
{
    float yy = kMenuPad;
    for (int i = 0; i < m.n; i++) {
        const float rh = MenuRowH(m.it[i]);
        if (y >= yy && y < yy + rh) return m.it[i].kind == MI_ITEM ? i : -1;
        yy += rh;
    }
    return -1;
}
void MenuPaint(HDC dc, int wpx, int hpx, float dpi, const Menu& m)
{
    if (!PopBegin(dc, wpx, hpx, dpi)) return;
    static const float check[6] = { 3.5f,8.5f, 6.5f,11.5f, 12.5f,4.5f };              // in una casella 16 x 16, come le icone della testata
    const float W = wpx * 96 / dpi, H = hpx * 96 / dpi, inset = MenuInset(m);
    g_rt->Clear(Col(C_SURF2)); Frame(0, 0, W, H, C_LINE);
    float yy = kMenuPad;
    for (int i = 0; i < m.n; i++) {
        const MenuItem& it = m.it[i]; const float rh = MenuRowH(it);
        if (it.kind == MI_SEP) R(8, floorf(yy + rh / 2), W - 16, 1, C_LINE);
        else if (it.kind == MI_HEAD) Text(F_LABEL, inset, yy + rh / 2 + 1, it.text, C_MUTED);
        else {
            if (i == m.hot) R(4, yy, W - 8, rh, C_TEXT, .09f);
            if (it.checked) Stroke(check, 3, 8, yy + 4, 1.4f, C_ACCENT);
            Text(F_TEXT, inset, yy + rh / 2, it.text, C_TEXT);
        }
        yy += rh;
    }
    PopEnd();
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
    g_hrt->Resize(D2D1::SizeU(rc.right, rc.bottom)); g_hrt->SetDpi(dpi, dpi); Rel(g_static);
}
void DrawThemeChanged() { if (g_hrt) ResCreate(g_hrt); }
void DrawPaint(HWND hwnd, const State& s, const Ui& ui)
{
    if (!g_hrt) return;
    g_rt = g_hrt;
    D2D1_SIZE_F sz = g_hrt->GetSize();
    g_hrt->BeginDraw(); DrawPanel(s, ui, sz.width, sz.height);
    if (g_hrt->EndDraw() == D2DERR_RECREATE_TARGET) { Rel(g_static); Rel(g_hrt); g_rt = nullptr; DrawCreate(hwnd); }   // il bersaglio si è perso: si rifà
}
bool DrawToDc(HDC memdc, int w, int h, float dpi, const State& s, const Ui& ui)
{
    if (!InitFactories()) return false;
    ID2D1DCRenderTarget* dc = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(g_f->CreateDCRenderTarget(&rp, &dc))) return false;
    RECT rc = { 0, 0, w, h };
    if (FAILED(dc->BindDC(memdc, &rc)) || !ResCreate(dc)) return false;
    dc->BeginDraw(); DrawPanel(s, ui, w * 96 / dpi, h * 96 / dpi);
    return SUCCEEDED(dc->EndDraw());
}

// ---- la finestra Aspetto (popups.cpp): quattro riquadri con la legenda sulla cornice, come quelli degli elenchi. I colori sono
// cinque tessere in fila col nome dentro; le altre scelte una riga l'una, con un cerchio pieno su quella in uso ----
static const float kApW = 372, kApH = 284;
struct ApBox { float x, y, w, h; const wchar_t* legend; };
static const ApBox kApBox[AP_GROUPS] = { { 14, 20, 344, 54, L"ACCENT" }, { 14, 92, 165, 92, L"THEME" }, { 193, 92, 165, 92, L"TRANSPARENCY" }, { 14, 202, 165, 68, L"DENSITY" } };
static const wchar_t* const kApLabel[AP_GROUPS][NACCENT] = {
    { L"green", L"amber", L"cyan", L"lime", L"Windows" }, { L"Dark", L"Light", L"Follow Windows" }, { L"Off", L"Low", L"High" }, { L"Compact", L"Comfortable" } };
static const int kApCount[AP_GROUPS] = { NACCENT, 3, 3, 2 };
struct ApOpt { int group, value; float x, y, w, h; };
static int ApOptions(ApOpt* o)
{
    int n = 0;
    for (int g = 0; g < AP_GROUPS; g++) for (int v = 0; v < kApCount[g]; v++) {
        const ApBox& b = kApBox[g]; ApOpt& p = o[n++]; p.group = g; p.value = v;
        if (g == AP_ACCENT) { p.w = 56; p.h = 30; p.x = b.x + 12 + v * 66.f; p.y = b.y + 12; }
        else { p.x = b.x + 1; p.w = b.w - 2; p.h = 24; p.y = b.y + 12 + v * 24.f; }
    }
    return n;
}
void AppearanceSize(float& w, float& h) { w = kApW; h = kApH; }
int AppearanceAt(float x, float y)
{
    ApOpt o[16]; const int n = ApOptions(o);
    for (int i = 0; i < n; i++) if (x >= o[i].x && x < o[i].x + o[i].w && y >= o[i].y && y < o[i].y + o[i].h) return o[i].group * 16 + o[i].value;
    return -1;
}
void AppearancePaint(HDC dc, int wpx, int hpx, float dpi, const Appearance& a)
{
    if (!PopBegin(dc, wpx, hpx, dpi)) return;
    g_rt->Clear(Col(C_BG));
    for (int g = 0; g < AP_GROUPS; g++) LegendFrame(kApBox[g].x, kApBox[g].y, kApBox[g].w, kApBox[g].h, kApBox[g].legend);
    ApOpt o[16]; const int n = ApOptions(o);
    for (int i = 0; i < n; i++) {
        const ApOpt& p = o[i]; const wchar_t* label = kApLabel[p.group][p.value];
        const bool on = a.value[p.group] == p.value, hot = a.hot == p.group * 16 + p.value;
        if (p.group == AP_ACCENT) {                                                  // la tessera in uso ha una cornice staccata, nel colore del testo
            const unsigned c = a.swatch[p.value];
            R(p.x, p.y, p.w, p.h, c);
            Text(F_MONO9, p.x + 5, p.y + p.h - 8, label, Luminance(c) > .3f ? 0x0b0d10 : 0xffffff);
            if (on || hot) Frame(p.x - 3, p.y - 3, p.w + 6, p.h + 6, on ? C_TEXT : C_MUTED);
            continue;
        }
        if (hot) R(p.x + 3, p.y, p.w - 6, p.h, C_TEXT, .07f);
        const D2D1_POINT_2F c = D2D1::Point2F(p.x + 16, p.y + p.h / 2);
        Smooth(true);
        g_br->SetColor(Col(on ? C_ACCENT : C_MUTED)); g_rt->DrawEllipse(D2D1::Ellipse(c, 5, 5), g_br, 1.3f);
        if (on) g_rt->FillEllipse(D2D1::Ellipse(c, 2.4f, 2.4f), g_br);
        Smooth(false);
        Text(F_TEXT, p.x + 30, p.y + p.h / 2, label, on ? C_TEXT : C_TEXT2);
    }
    Text(F_SMALL, 193, 216, L"Changes apply at once", C_MUTED); Text(F_SMALL, 193, 232, L"and are saved.", C_MUTED);
    PopEnd();
}
