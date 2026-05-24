using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;
using Pen = System.Windows.Media.Pen;
using Point = System.Windows.Point;

namespace ResourceMonitor.Controls;

public sealed class Sparkline : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty =
        DependencyProperty.Register(nameof(Values), typeof(IEnumerable<double>), typeof(Sparkline),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty MaxValueProperty =
        DependencyProperty.Register(nameof(MaxValue), typeof(double), typeof(Sparkline),
            new FrameworkPropertyMetadata(100d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty AutoScaleProperty =
        DependencyProperty.Register(nameof(AutoScale), typeof(bool), typeof(Sparkline),
            new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrokeProperty =
        DependencyProperty.Register(nameof(Stroke), typeof(Brush), typeof(Sparkline),
            new FrameworkPropertyMetadata(Brushes.DeepSkyBlue, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrokeThicknessProperty =
        DependencyProperty.Register(nameof(StrokeThickness), typeof(double), typeof(Sparkline),
            new FrameworkPropertyMetadata(1.6, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FillOpacityProperty =
        DependencyProperty.Register(nameof(FillOpacity), typeof(double), typeof(Sparkline),
            new FrameworkPropertyMetadata(0.30, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty ShowEndDotProperty =
        DependencyProperty.Register(nameof(ShowEndDot), typeof(bool), typeof(Sparkline),
            new FrameworkPropertyMetadata(true, FrameworkPropertyMetadataOptions.AffectsRender));

    public IEnumerable<double>? Values
    {
        get => (IEnumerable<double>?)GetValue(ValuesProperty);
        set => SetValue(ValuesProperty, value);
    }

    public double MaxValue
    {
        get => (double)GetValue(MaxValueProperty);
        set => SetValue(MaxValueProperty, value);
    }

    public bool AutoScale
    {
        get => (bool)GetValue(AutoScaleProperty);
        set => SetValue(AutoScaleProperty, value);
    }

    public Brush Stroke
    {
        get => (Brush)GetValue(StrokeProperty);
        set => SetValue(StrokeProperty, value);
    }

    public double StrokeThickness
    {
        get => (double)GetValue(StrokeThicknessProperty);
        set => SetValue(StrokeThicknessProperty, value);
    }

    public double FillOpacity
    {
        get => (double)GetValue(FillOpacityProperty);
        set => SetValue(FillOpacityProperty, value);
    }

    public bool ShowEndDot
    {
        get => (bool)GetValue(ShowEndDotProperty);
        set => SetValue(ShowEndDotProperty, value);
    }

    public void Invalidate() => InvalidateVisual();

    // Buffer riutilizzato fra render (Point[] e Brush/Pen cache).
    private Point[]? _pointsBuf;
    private SolidColorBrush? _haloBrushCache;
    private Color _haloBrushColor;
    private LinearGradientBrush? _fillBrushCache;
    private Color _fillBrushTopColor;

    protected override void OnRender(DrawingContext dc)
    {
        var values = Values;
        if (values is null) return;

        int n;
        if (values is IReadOnlyCollection<double> coll) n = coll.Count;
        else { n = 0; foreach (var _ in values) n++; }
        if (n < 2) return;

        double w = ActualWidth;
        double h = ActualHeight;
        if (w <= 0 || h <= 0) return;

        // Calcola max (autoscale) o usa MaxValue. Iterate enumerator, no ToArray.
        double max;
        if (AutoScale)
        {
            max = 1;
            foreach (var v in values) if (v > max) max = v;
        }
        else
        {
            max = MaxValue > 0 ? MaxValue : 1;
        }

        // Riusa buffer se possibile
        if (_pointsBuf is null || _pointsBuf.Length < n) _pointsBuf = new Point[n];
        var points = _pointsBuf;

        double stepX = w / (n - 1);
        const double pad = 2;
        double usableH = h - pad * 2;
        int idx = 0;
        foreach (var v in values)
        {
            if (idx >= n) break;
            double norm = Math.Clamp(v / max, 0, 1);
            points[idx] = new Point(idx * stepX, h - pad - norm * usableH);
            idx++;
        }

        var lineColor = GetStrokeColor();

        // FILL — gradient verticale (riutilizziamo il brush se il colore non cambia)
        var fillTop = Color.FromArgb((byte)(FillOpacity * 255), lineColor.R, lineColor.G, lineColor.B);
        if (_fillBrushCache is null || _fillBrushTopColor != fillTop)
        {
            var fillBot = Color.FromArgb(0, lineColor.R, lineColor.G, lineColor.B);
            _fillBrushCache = new LinearGradientBrush(fillTop, fillBot, 90);
            if (_fillBrushCache.CanFreeze) _fillBrushCache.Freeze();
            _fillBrushTopColor = fillTop;
        }

        var fillGeo = new StreamGeometry();
        using (var ctx = fillGeo.Open())
        {
            ctx.BeginFigure(new Point(0, h), true, true);
            ctx.LineTo(points[0], false, false);
            AppendSmoothCurve(ctx, points, n);
            ctx.LineTo(new Point(w, h), false, false);
        }
        fillGeo.Freeze();
        dc.DrawGeometry(_fillBrushCache, null, fillGeo);

        // STROKE
        var lineGeo = new StreamGeometry();
        using (var ctx = lineGeo.Open())
        {
            ctx.BeginFigure(points[0], false, false);
            AppendSmoothCurve(ctx, points, n);
        }
        lineGeo.Freeze();

        var pen = new Pen(Stroke, StrokeThickness)
        {
            LineJoin = PenLineJoin.Round,
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round
        };
        if (pen.CanFreeze) pen.Freeze();
        dc.DrawGeometry(null, pen, lineGeo);

        // DOT on last value
        if (ShowEndDot)
        {
            var last = points[n - 1];
            double dotR = StrokeThickness * 1.6;

            var haloColor = Color.FromArgb(70, lineColor.R, lineColor.G, lineColor.B);
            if (_haloBrushCache is null || _haloBrushColor != haloColor)
            {
                _haloBrushCache = new SolidColorBrush(haloColor);
                if (_haloBrushCache.CanFreeze) _haloBrushCache.Freeze();
                _haloBrushColor = haloColor;
            }
            dc.DrawEllipse(_haloBrushCache, null, last, dotR * 2.0, dotR * 2.0);
            dc.DrawEllipse(Stroke, null, last, dotR, dotR);
        }
    }

    private static void AppendSmoothCurve(StreamGeometryContext ctx, Point[] points, int count)
    {
        const double tension = 0.5;
        for (int i = 0; i < count - 1; i++)
        {
            Point p0 = i == 0 ? points[i] : points[i - 1];
            Point p1 = points[i];
            Point p2 = points[i + 1];
            Point p3 = i + 2 < count ? points[i + 2] : points[i + 1];

            var c1 = new Point(
                p1.X + (p2.X - p0.X) * tension / 3.0,
                p1.Y + (p2.Y - p0.Y) * tension / 3.0);
            var c2 = new Point(
                p2.X - (p3.X - p1.X) * tension / 3.0,
                p2.Y - (p3.Y - p1.Y) * tension / 3.0);

            ctx.BezierTo(c1, c2, p2, true, false);
        }
    }

    private Color GetStrokeColor()
    {
        if (Stroke is SolidColorBrush scb) return scb.Color;
        return Colors.DeepSkyBlue;
    }
}
