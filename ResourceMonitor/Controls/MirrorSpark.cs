using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Pen = System.Windows.Media.Pen;
using Point = System.Windows.Point;

namespace ResourceMonitor.Controls;

// rete e disco a specchio: quello che entra (scaricato, letto) sopra la linea di mezzo, quello che esce (inviato, scritto) sotto,
// tutti e due sulla stessa scala fissa (MaxValue), che chi ospita il grafico scrive accanto. Con una scala che non insegue il
// picco, un minuto tranquillo resta piatto e un picco vero si vede. Linee dritte tra un secondo e l'altro: sono valori a scatti,
// una curva li farebbe sembrare più alti di quanto sono. Le righe della griglia (GridStep) cadono dove cadono in Sparkline
public sealed class MirrorSpark : FrameworkElement
{
    public static readonly DependencyProperty AboveProperty =
        DependencyProperty.Register(nameof(Above), typeof(IEnumerable<double>), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty BelowProperty =
        DependencyProperty.Register(nameof(Below), typeof(IEnumerable<double>), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty MaxValueProperty =
        DependencyProperty.Register(nameof(MaxValue), typeof(double), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(1d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrokeProperty =
        DependencyProperty.Register(nameof(Stroke), typeof(Brush), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(Brushes.Goldenrod, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty GridStepProperty =
        DependencyProperty.Register(nameof(GridStep), typeof(int), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(0, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty GridBrushProperty =
        DependencyProperty.Register(nameof(GridBrush), typeof(Brush), typeof(MirrorSpark),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public IEnumerable<double>? Above
    {
        get => (IEnumerable<double>?)GetValue(AboveProperty);
        set => SetValue(AboveProperty, value);
    }

    public IEnumerable<double>? Below
    {
        get => (IEnumerable<double>?)GetValue(BelowProperty);
        set => SetValue(BelowProperty, value);
    }

    public double MaxValue
    {
        get => (double)GetValue(MaxValueProperty);
        set => SetValue(MaxValueProperty, value);
    }

    public Brush Stroke
    {
        get => (Brush)GetValue(StrokeProperty);
        set => SetValue(StrokeProperty, value);
    }

    /// <summary>Una riga verticale ogni tanti campioni, contati dal più recente; 0 = nessuna.</summary>
    public int GridStep
    {
        get => (int)GetValue(GridStepProperty);
        set => SetValue(GridStepProperty, value);
    }

    public Brush? GridBrush
    {
        get => (Brush?)GetValue(GridBrushProperty);
        set => SetValue(GridBrushProperty, value);
    }

    private Point[]? _points;
    private SolidColorBrush? _fillAbove, _fillBelow, _lineBelow, _middle;
    private Pen? _penAbove, _penBelow;
    private Color _color;

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        double scale = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        int n = Count(Above);
        Sparkline.DrawGrid(dc, GridBrush, GridStep, n, w, h, scale);
        if (n < 2) return;

        var color = Stroke is SolidColorBrush solid ? solid.Color : Colors.Goldenrod;
        if (_fillAbove is null || _color != color)
        {
            _fillAbove = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.30));
            _fillBelow = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.14));
            _lineBelow = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.70));
            _middle = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.40));
            _penAbove = new Pen(Reticle.Wpf.Palette.Freeze(color), 1.2) { LineJoin = PenLineJoin.Round };
            _penAbove.Freeze();
            _penBelow = new Pen(_lineBelow, 1.2) { LineJoin = PenLineJoin.Round };
            _penBelow.Freeze();
            _color = color;
        }

        double max = MaxValue > 0 ? MaxValue : 1;
        // la linea di mezzo su un pixel dello schermo; un pixel di stacco da lei, tre dal bordo
        double middle = Math.Round(h / 2 * scale) / scale, line = 1 / scale;
        Half(dc, Above, n, w, max, middle, -(middle - 3), _fillAbove, _penAbove!);
        if (Count(Below) == n) Half(dc, Below, n, w, max, middle + line, h - middle - line - 3, _fillBelow!, _penBelow!);
        dc.DrawRectangle(_middle, null, new Rect(0, middle, w, line));
    }

    // una metà: l'area tra la linea di mezzo e i valori (span negativo = verso l'alto), poi il suo contorno
    private void Half(DrawingContext dc, IEnumerable<double>? values, int n, double w, double max, double baseline, double span, Brush fill, Pen pen)
    {
        if (values is null) return;
        if (_points is null || _points.Length < n) _points = new Point[n];
        double step = w / (n - 1), away = span < 0 ? -1 : 1;
        int i = 0;
        foreach (double v in values)
        {
            if (i >= n) break;
            double norm = double.IsNaN(v) ? 0 : Math.Clamp(v / max, 0, 1);
            _points[i] = new Point(i * step, baseline + away + norm * span);
            i++;
        }
        var area = new StreamGeometry();
        using (var ctx = area.Open())
        {
            ctx.BeginFigure(new Point(0, baseline), true, true);
            for (int k = 0; k < n; k++) ctx.LineTo(_points[k], false, false);
            ctx.LineTo(new Point(w, baseline), false, false);
        }
        area.Freeze();
        dc.DrawGeometry(fill, null, area);
        var outline = new StreamGeometry();
        using (var ctx = outline.Open())
        {
            ctx.BeginFigure(_points[0], false, false);
            for (int k = 1; k < n; k++) ctx.LineTo(_points[k], true, true);
        }
        outline.Freeze();
        dc.DrawGeometry(null, pen, outline);
    }

    private static int Count(IEnumerable<double>? values)
    {
        if (values is null) return 0;
        if (values is IReadOnlyCollection<double> known) return known.Count;
        int n = 0;
        foreach (var _ in values) n++;
        return n;
    }
}
