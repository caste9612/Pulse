param(
    [string]$ExePath = "C:\Users\wcast\Desktop\resourcemonitor\dist\Pulse.exe",
    [string]$Label = "current",
    [switch]$ClearCache,
    [int]$DurationSec = 120
)

# Kill any running instance
Get-Process -Name Pulse,ResourceMonitor -ErrorAction SilentlyContinue | ForEach-Object {
    try { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue } catch {}
}
Start-Sleep -Milliseconds 800

if ($ClearCache) {
    $cacheDir = Join-Path $env:LOCALAPPDATA "Temp\.net\Pulse"
    if (Test-Path $cacheDir) {
        Remove-Item -Recurse -Force $cacheDir -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "================== $Label ==================" -ForegroundColor Cyan
$exeFile = Get-Item $ExePath
Write-Host ("Exe size: {0:N2} MB" -f ($exeFile.Length / 1MB))
$distDir = Split-Path $ExePath -Parent
$dllSize = (Get-ChildItem $distDir -Filter "*.dll" -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("Native DLLs: {0:N2} MB" -f $dllSize)

# Start with stopwatch
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $ExePath -PassThru
$inputIdleMs = -1
try {
    $proc.WaitForInputIdle(15000) | Out-Null
    $inputIdleMs = $sw.ElapsedMilliseconds
} catch {}
Write-Host ("Cold start (msg loop ready): {0} ms" -f $inputIdleMs) -ForegroundColor Green

# Wait for settle (25s skip startup spike + LHM init)
Write-Host "Waiting 25s for settle..."
Start-Sleep -Seconds 25

# Take initial sample
$proc.Refresh()
$ramStart_WS = $proc.WorkingSet64 / 1MB
$ramStart_Priv = $proc.PrivateMemorySize64 / 1MB
$ramStart_Handles = $proc.HandleCount
$ramStart_Threads = $proc.Threads.Count
Write-Host ""
Write-Host "=== Snapshot iniziale (dopo 25s) ===" -ForegroundColor Yellow
Write-Host ("  Working Set:   {0,7:N1} MB" -f $ramStart_WS)
Write-Host ("  Private:       {0,7:N1} MB" -f $ramStart_Priv)
Write-Host ("  Handles:       {0,7}"   -f $ramStart_Handles)
Write-Host ("  Threads:       {0,7}"   -f $ramStart_Threads)

# Now sample CPU + RAM over duration
Write-Host ""
Write-Host ("Campionamento per ${DurationSec}s...")
$samples = @()
$cpuPrev = $proc.TotalProcessorTime.TotalSeconds
$wallPrev = Get-Date
$ramPeakWS = $ramStart_WS
$ramMinWS = $ramStart_WS

for ($i = 1; $i -le $DurationSec; $i++) {
    Start-Sleep -Seconds 1
    $proc.Refresh()
    $cpuNow = $proc.TotalProcessorTime.TotalSeconds
    $wallNow = Get-Date
    $cpuPct = (($cpuNow - $cpuPrev) / ($wallNow - $wallPrev).TotalSeconds) * 100
    $cpuPrev = $cpuNow
    $wallPrev = $wallNow
    $wsNow = $proc.WorkingSet64 / 1MB
    if ($wsNow -gt $ramPeakWS) { $ramPeakWS = $wsNow }
    if ($wsNow -lt $ramMinWS) { $ramMinWS = $wsNow }
    $samples += [PSCustomObject]@{
        T = $i
        CpuPct = [math]::Round($cpuPct, 2)
        WS = [math]::Round($wsNow, 1)
        Priv = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
        Handles = $proc.HandleCount
        Threads = $proc.Threads.Count
    }
    if ($i % 30 -eq 0) {
        Write-Host ("  [{0,3}s] CPU={1,5:N2}%  WS={2,6:N1}MB  Handles={3,5}  Threads={4,3}" -f $i, $cpuPct, $wsNow, $proc.HandleCount, $proc.Threads.Count)
    }
}

# Stats
$cpuAvg = ($samples | Measure-Object CpuPct -Average).Average
$cpuP95 = ($samples | Sort-Object CpuPct)[[int]($samples.Count * 0.95)].CpuPct
$cpuMax = ($samples | Measure-Object CpuPct -Maximum).Maximum
$wsAvg = ($samples | Measure-Object WS -Average).Average
$wsEnd = $samples[-1].WS
$wsGrowth = $wsEnd - $samples[0].WS
$handlesEnd = $samples[-1].Handles
$handlesStart = $samples[0].Handles

Write-Host ""
Write-Host "=== Risultati ===" -ForegroundColor Green
Write-Host ("  CPU media:        {0,5:N2}%" -f $cpuAvg)
Write-Host ("  CPU P95:          {0,5:N2}%" -f $cpuP95)
Write-Host ("  CPU max:          {0,5:N2}%" -f $cpuMax)
Write-Host ("  RAM WS media:     {0,6:N1} MB" -f $wsAvg)
Write-Host ("  RAM WS min/max:   {0,6:N1} / {1:N1} MB" -f $ramMinWS, $ramPeakWS)
$sign = if ($wsGrowth -ge 0) { "+" } else { "" }
Write-Host ("  RAM crescita:     {0}{1:N1} MB ({2}s)" -f $sign, $wsGrowth, $DurationSec)
Write-Host ("  Handles start:    {0}" -f $handlesStart)
Write-Host ("  Handles end:      {0} (delta {1:+0;-0;0})" -f $handlesEnd, ($handlesEnd - $handlesStart))

# Save result
$row = [PSCustomObject]@{
    Label = $Label
    Timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    ExeMB = [math]::Round($exeFile.Length / 1MB, 2)
    DllMB = [math]::Round($dllSize, 2)
    ColdStartMs = $inputIdleMs
    RamStartMB = [math]::Round($ramStart_Priv, 1)
    WsAvgMB = [math]::Round($wsAvg, 1)
    WsPeakMB = [math]::Round($ramPeakWS, 1)
    WsGrowthMB = [math]::Round($wsGrowth, 1)
    CpuAvgPct = [math]::Round($cpuAvg, 2)
    CpuP95Pct = [math]::Round($cpuP95, 2)
    CpuMaxPct = [math]::Round($cpuMax, 2)
    HandlesEnd = $handlesEnd
    HandlesDelta = $handlesEnd - $handlesStart
    Threads = $samples[-1].Threads
}

$csvPath = "C:\Users\wcast\Desktop\resourcemonitor\perf-deep.csv"
if (-not (Test-Path $csvPath)) {
    $row | Export-Csv -Path $csvPath -NoTypeInformation
} else {
    $row | Export-Csv -Path $csvPath -NoTypeInformation -Append
}
Write-Host ""
Write-Host "Salvato in $csvPath" -ForegroundColor Gray
return $row
