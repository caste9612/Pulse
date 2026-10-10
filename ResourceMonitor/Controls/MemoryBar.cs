using System;
using System.Windows;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;

namespace ResourceMonitor.Controls;

// la composizione della RAM come la barra di Gestione attività: da sinistra in uso, modificata, in cache, libera, nel colore della
// RAM a intensità che calano (pieno, 60 %, 30 %, e la libera come traccia al 10 %, la stessa delle barre dei core); 1 px di stacco
// tra una parte e l'altra, così due toni vicini restano distinti. Bordi sui pixel dello schermo, come in CoreBars
public sealed class MemoryBar : FrameworkElement
{
    public static readonly DependencyProperty PartsProperty =
        DependencyProperty.Register(nameof(Parts), typeof(double[]), typeof(MemoryBar),
            new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FillProperty =
        DependencyProperty.Register(nameof(Fill), typeof(Brush), typeof(MemoryBar),
            new FrameworkPropertyMetadata(Brushes.MediumPurple, FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>Le parti in GB, da sinistra: in uso, modificata, in cache, libera.</summary>
    public double[]? Parts
    {
        get => (double[]?)GetValue(PartsProperty);
        set => SetValue(PartsProperty, value);
    }

    public Brush Fill
    {
        get => (Brush)GetValue(FillProperty);
        set => SetValue(FillProperty, value);
    }

    private static readonly double[] Alphas = { 1.0, 0.6, 0.3, 0.10 };
    private readonly SolidColorBrush[] _brushes = new SolidColorBrush[Alphas.Length];
    private Color _brushesColor;

    protected override void OnRender(DrawingContext dc)
    {
        var parts = Parts;
        double w = ActualWidth;
        double h = ActualHeight;
        if (parts is null || parts.Length < Alphas.Length || w <= 0 || h <= 0) return;
        double total = 0;
        for (int i = 0; i < Alphas.Length; i++) total += Math.Max(0, parts[i]);
        if (total <= 0) return;

        var color = Fill is SolidColorBrush solid ? solid.Color : Colors.MediumPurple;
        if (_brushes[0] is null || _brushesColor != color)
        {
            for (int i = 0; i < Alphas.Length; i++) _brushes[i] = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, Alphas[i]));
            _brushesColor = color;
        }

        // fondo trasparente: il tooltip risponde su tutta la barra
        dc.DrawRectangle(Brushes.Transparent, null, new Rect(0, 0, w, h));
        double scale = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        double width = w * scale, sum = 0, left = 0;
        for (int i = 0; i < Alphas.Length; i++)
        {
            sum += Math.Max(0, parts[i]);
            double right = Math.Round(width * sum / total);
            // lo stacco si prende dall'inizio della parte, se ce n'è una prima
            double start = left > 0 ? left + 1 : left;
            if (right > start) dc.DrawRectangle(_brushes[i], null, new Rect(start / scale, 0, (right - start) / scale, h));
            if (right > left) left = right;
        }
    }
}
