# Genera l'icona Pulse (ECG heartbeat) come .ico multi-risoluzione
# Esegui una volta sola dopo aver cambiato il design.
# Output: ResourceMonitor\Resources\app.ico

Add-Type -AssemblyName System.Drawing

$outDir = Join-Path $PSScriptRoot "..\ResourceMonitor\Resources"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outIco = Join-Path $outDir "app.ico"

# Risoluzioni da includere nell'.ico
$sizes = @(16, 24, 32, 48, 64, 128, 256)

function Draw-PulseLogo {
    param([int]$size)

    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    # Background trasparente
    $g.Clear([System.Drawing.Color]::Transparent)

    # Cerchio scuro di sfondo
    $bgColor = [System.Drawing.Color]::FromArgb(240, 27, 27, 35)
    $bgBrush = New-Object System.Drawing.SolidBrush $bgColor
    $inset = [math]::Max(1, [int]($size * 0.04))
    $g.FillEllipse($bgBrush, $inset, $inset, $size - 2*$inset, $size - 2*$inset)

    # Bordo sottile
    $borderColor = [System.Drawing.Color]::FromArgb(60, 255, 255, 255)
    $borderPen = New-Object System.Drawing.Pen $borderColor, ([math]::Max(0.5, $size * 0.008))
    $g.DrawEllipse($borderPen, $inset, $inset, $size - 2*$inset, $size - 2*$inset)

    # Linea ECG heartbeat in cyan
    $ecgColor = [System.Drawing.Color]::FromArgb(255, 111, 207, 232)  # CpuBrush #6FCFE8
    $strokeWidth = [math]::Max(1.0, $size * 0.06)
    $pen = New-Object System.Drawing.Pen $ecgColor, $strokeWidth
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    # Coordinate ECG (in 0-1, scaleremo)
    $center = $size / 2.0
    $halfH = $size * 0.18  # ampiezza picco
    $points = @(
        @(0.10, 0.50),  # baseline sinistra
        @(0.32, 0.50),
        @(0.38, 0.58),  # piccola depressione (onda P)
        @(0.44, 0.50),
        @(0.50, 0.22),  # grande picco (R)
        @(0.55, 0.78),  # grande discesa (S)
        @(0.62, 0.50),
        @(0.68, 0.50),  # back to baseline
        @(0.90, 0.50)
    )

    $drawingPoints = New-Object System.Drawing.PointF[] $points.Count
    for ($i = 0; $i -lt $points.Count; $i++) {
        $drawingPoints[$i] = New-Object System.Drawing.PointF (
            [float]($points[$i][0] * $size),
            [float]($points[$i][1] * $size)
        )
    }
    $g.DrawLines($pen, $drawingPoints)

    $pen.Dispose()
    $borderPen.Dispose()
    $bgBrush.Dispose()
    $g.Dispose()
    return $bmp
}

function Save-Ico {
    param([System.Drawing.Bitmap[]]$bitmaps, [string]$path)

    # Encode ognuna come PNG in memoria
    $pngBytes = @()
    foreach ($bmp in $bitmaps) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngBytes += , $ms.ToArray()
        $ms.Dispose()
    }

    $count = $bitmaps.Count
    $headerSize = 6
    $dirEntrySize = 16
    $imagesOffset = $headerSize + ($dirEntrySize * $count)

    $stream = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $stream

    # ICONDIR
    $w.Write([UInt16]0)      # Reserved
    $w.Write([UInt16]1)      # Type ICO
    $w.Write([UInt16]$count) # Image count

    # Per ogni dir entry
    $offset = $imagesOffset
    for ($i = 0; $i -lt $count; $i++) {
        $bmp = $bitmaps[$i]
        $size = $bmp.Width
        $w.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))   # Width
        $w.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))   # Height
        $w.Write([Byte]0)        # ColorCount
        $w.Write([Byte]0)        # Reserved
        $w.Write([UInt16]1)      # Planes
        $w.Write([UInt16]32)     # BitCount
        $w.Write([UInt32]$pngBytes[$i].Length)
        $w.Write([UInt32]$offset)
        $offset += $pngBytes[$i].Length
    }

    # Image data
    foreach ($bytes in $pngBytes) {
        $w.Write($bytes)
    }

    [System.IO.File]::WriteAllBytes($path, $stream.ToArray())
    $w.Dispose()
    $stream.Dispose()
}

Write-Host "Genero icona Pulse a $($sizes.Count) risoluzioni..."
$bitmaps = @()
foreach ($s in $sizes) {
    $bitmaps += Draw-PulseLogo -size $s
    Write-Host "  ${s}x${s}"
}

Save-Ico -bitmaps $bitmaps -path $outIco
$bitmaps | ForEach-Object { $_.Dispose() }

$file = Get-Item $outIco
Write-Host "Salvato: $outIco ($($file.Length) bytes)" -ForegroundColor Green
