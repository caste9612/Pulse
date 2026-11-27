using System;
using System.Windows;
using System.Windows.Media;
using Brush = System.Windows.Media.Brush;
using Brushes = System.Windows.Media.Brushes;

namespace ResourceMonitor.Controls;

// il livello di adesso, dietro il testo di una metrica: il colore della serie si stende da sinistra in proporzione al valore, leggero
// (una velatura: il testo sopra resta leggibile), pieno solo sul bordo basso; una tacca segna il massimo dell'ultimo minuto.
// Nessuna riga verticale attraversa le scritte. Bordi sui pixel dello schermo, come in CoreBars
public sealed class LevelTint : FrameworkElement
{
    public static readonly DependencyProperty ValueProperty =
        DependencyProperty.Register(nameof(Value), typeof(double), typeof(LevelTint),
            new FrameworkPropertyMetadata(0d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty PeakProperty =
        DependencyProperty.Register(nameof(Peak), typeof(double), typeof(LevelTint),
            new FrameworkPropertyMetadata(double.NaN, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty FillProperty =
        DependencyProperty.Register(nameof(Fill), typeof(Brush), typeof(LevelTint),
            new FrameworkPropertyMetadata(Brushes.DeepSkyBlue, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty TickProperty =
        DependencyProperty.Register(nameof(Tick), typeof(Brush), typeof(LevelTint),
            new FrameworkPropertyMetadata(Brushes.White, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty StrengthProperty =
        DependencyProperty.Register(nameof(Strength), typeof(double), typeof(LevelTint),
            new FrameworkPropertyMetadata(1d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty TickWidthProperty =
        DependencyProperty.Register(nameof(TickWidth), typeof(double), typeof(LevelTint),
            new FrameworkPropertyMetadata(2d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty TickHeightProperty =
        DependencyProperty.Register(nameof(TickHeight), typeof(double), typeof(LevelTint),
            new FrameworkPropertyMetadata(6d, FrameworkPropertyMetadataOptions.AffectsRender));

    /// <summary>Il valore di adesso, da 0 a 1.</summary>
    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    /// <summary>Il massimo del periodo mostrato nel grafico, da 0 a 1; NaN = nessuna tacca.</summary>
    public double Peak
    {
        get => (double)GetValue(PeakProperty);
        set => SetValue(PeakProperty, value);
    }

    public Brush Fill
    {
        get => (Brush)GetValue(FillProperty);
        set => SetValue(FillProperty, value);
    }

    public Brush Tick
    {
        get => (Brush)GetValue(TickProperty);
        set => SetValue(TickProperty, value);
    }

    /// <summary>Quanto è carico il colore, da 0 a 1: la seconda di due velature vicine (l'uscita sotto l'entrata) sta più bassa.</summary>
    public double Strength
    {
        get => (double)GetValue(StrengthProperty);
        set => SetValue(StrengthProperty, value);
    }

    /// <summary>La misura della tacca del massimo: stretta e alta 6 dove sotto il testo c'è spazio (una riga da 32); dove due
    /// velature stanno una sull'altra in righe da 16 le cifre arrivano quasi al bordo, e la tacca è un trattino alto quanto il
    /// bordo pieno, così non finisce sopra un numero.</summary>
    public double TickWidth
    {
        get => (double)GetValue(TickWidthProperty);
        set => SetValue(TickWidthProperty, value);
    }

    public double TickHeight
    {
        get => (double)GetValue(TickHeightProperty);
        set => SetValue(TickHeightProperty, value);
    }

    private SolidColorBrush? _track, _tint, _line;
    private Color _color;
    private double _strength = -1;

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0) return;
        var color = Fill is SolidColorBrush solid ? solid.Color : Colors.DeepSkyBlue;
        double strength = Math.Clamp(Strength, 0, 1);
        if (_track is null || _color != color || _strength != strength)
        {
            _track = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.05));
            _tint = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, 0.18 * strength));
            _line = Reticle.Wpf.Palette.Freeze(Reticle.Wpf.Palette.WithAlpha(color, strength));
            _color = color;
            _strength = strength;
        }

        double scale = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        double width = Math.Round(w * scale), height = Math.Round(h * scale);
        double value = double.IsNaN(Value) ? 0 : Math.Clamp(Value, 0, 1);
        double fill = Math.Round(width * value), edge = Math.Round(2 * scale);

        dc.DrawRectangle(_track, null, new Rect(0, 0, width / scale, height / scale));
        if (fill > 0)
        {
            dc.DrawRectangle(_tint, null, new Rect(0, 0, fill / scale, height / scale));
            dc.DrawRectangle(_line, null, new Rect(0, (height - edge) / scale, fill / scale, edge / scale));
        }
        if (!double.IsNaN(Peak))
        {
            double tick = Math.Max(1, Math.Round(TickWidth * scale)), tall = Math.Max(1, Math.Round(TickHeight * scale));
            double x = Math.Clamp(Math.Round(width * Math.Clamp(Peak, 0, 1)) - tick, 0, width - tick);
            dc.DrawRectangle(Tick, null, new Rect(x / scale, (height - tall) / scale, tick / scale, tall / scale));
        }
    }
}
