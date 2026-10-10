using System;
using System.Windows;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;

namespace ResourceMonitor.Controls;

// il carico di ogni core logico, una barra ciascuno che si riempie dal basso: i thread dello stesso core fisico stanno attaccati, tra
// un core fisico e l'altro c'è uno stacco. Con le quote dei processi seguiti (Shares) il riempimento va a strati: dal basso ogni
// processo nel suo colore, sopra il resto. La traccia è il colore delle barre al 10 % (il meter di Reticle ne ha una sola al 15 %:
// trentadue affiancate pesano di più); i bordi cadono sui pixel dello schermo, così le barre strette restano nitide a ogni scala
public sealed class CoreBars : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty =
        DependencyProperty.Register(nameof(Values), typeof(double[]), typeof(CoreBars),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty CoresProperty =
        DependencyProperty.Register(nameof(Cores), typeof(int[]), typeof(CoreBars),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FillProperty =
        DependencyProperty.Register(nameof(Fill), typeof(Brush), typeof(CoreBars),
            new FrameworkPropertyMetadata(Brushes.DeepSkyBlue, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty SharesProperty =
        DependencyProperty.Register(nameof(Shares), typeof(double[]), typeof(CoreBars),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty ShareFillsProperty =
        DependencyProperty.Register(nameof(ShareFills), typeof(Brush[]), typeof(CoreBars),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>Carico di ogni core logico, 0–100.</summary>
    public double[]? Values
    {
        get => (double[]?)GetValue(ValuesProperty);
        set => SetValue(ValuesProperty, value);
    }

    /// <summary>Per ogni valore, il core fisico a cui appartiene; null = ogni core logico fa core a sé.</summary>
    public int[]? Cores
    {
        get => (int[]?)GetValue(CoresProperty);
        set => SetValue(CoresProperty, value);
    }

    public Brush Fill
    {
        get => (Brush)GetValue(FillProperty);
        set => SetValue(FillProperty, value);
    }

    /// <summary>Per ogni core, la parte del suo carico di ciascun processo seguito (tanti valori per core quanti
    /// <see cref="ShareFills"/>, 0–1): strati dal basso nei loro colori, il resto in <see cref="Fill"/>. Null = tutto in Fill.</summary>
    public double[]? Shares
    {
        get => (double[]?)GetValue(SharesProperty);
        set => SetValue(SharesProperty, value);
    }

    public Brush[]? ShareFills
    {
        get => (Brush[]?)GetValue(ShareFillsProperty);
        set => SetValue(ShareFillsProperty, value);
    }

    private SolidColorBrush? _trackCache;

    protected override void OnRender(DrawingContext dc)
    {
        var values = Values;
        double w = ActualWidth;
        double h = ActualHeight;
        if (values is null || values.Length == 0 || w <= 0 || h <= 0) return;
        var cores = Cores;
        int n = values.Length;

        // tutto in pixel dello schermo: stacco tra due core fisici fino a 4 px (meno se le barre sono strette), tra due thread dello
        // stesso core 1 px quando c'è posto
        double scale = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        double width = w * scale, height = h * scale;
        int breaks = 0;
        for (int i = 1; i < n; i++) if (NewCore(cores, i)) breaks++;
        double coreGap = Math.Clamp(Math.Floor(width / n * 0.4), 1, Math.Round(4 * scale));
        double threadGap = coreGap >= 3 ? Math.Round(scale) : 0;
        double bar = (width - breaks * coreGap - (n - 1 - breaks) * threadGap) / n;
        if (bar < 1) return;

        var fill = Fill;
        var color = fill is SolidColorBrush solid ? solid.Color : Colors.DeepSkyBlue;
        var track = Reticle.Wpf.Palette.WithAlpha(color, 0.10);
        if (_trackCache is null || _trackCache.Color != track) _trackCache = Reticle.Wpf.Palette.Freeze(track);

        var shares = Shares;
        var shareFills = ShareFills;
        int slots = shares is not null && shareFills is not null && shares.Length >= n * shareFills.Length ? shareFills.Length : 0;

        // fondo trasparente: il tooltip risponde su tutta la fila, stacchi compresi
        dc.DrawRectangle(Brushes.Transparent, null, new Rect(0, 0, w, h));
        double x = 0;
        for (int i = 0; i < n; i++)
        {
            if (i > 0) x += NewCore(cores, i) ? coreGap : threadGap;
            double left = Math.Round(x), right = Math.Round(x + bar);
            x += bar;
            if (right <= left) continue;
            double top = Math.Round(height * (1 - Math.Clamp(values[i] / 100, 0, 1)));
            if (top > 0) Draw(dc, _trackCache, left, 0, right, top, scale);
            // gli strati dei processi seguiti dal basso, con i bordi sui pixel: niente fessure tra uno strato e l'altro
            double bottom = height, share = 0;
            for (int k = 0; k < slots; k++)
            {
                share += Math.Clamp(shares![i * slots + k], 0, 1);
                double layerTop = Math.Max(top, Math.Round(height - (height - top) * Math.Min(share, 1)));
                Draw(dc, shareFills![k], left, layerTop, right, bottom, scale);
                bottom = layerTop;
            }
            Draw(dc, fill, left, top, right, bottom, scale);
        }
    }

    private static void Draw(DrawingContext dc, Brush brush, double left, double top, double right, double bottom, double scale)
    {
        if (bottom > top) dc.DrawRectangle(brush, null, new Rect(left / scale, top / scale, (right - left) / scale, (bottom - top) / scale));
    }

    private static bool NewCore(int[]? cores, int i) => cores is null || i >= cores.Length || cores[i] != cores[i - 1];
}
