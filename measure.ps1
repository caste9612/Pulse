param(
    [string]$ExePath = "C:\Users\wcast\Desktop\resourcemonitor\dist\ResourceMonitor.exe",
    [string]$Label = "current",
    [switch]$ClearCache,
    [int]$IdleSampleSeconds = 30
)

# Kill any running instance
Get-Process -Name ResourceMonitor -ErrorAction SilentlyContinue | ForEach-Object {
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Milliseconds 800

if ($ClearCache) {
    $cacheDir = Join-Path $env:LOCALAPPDATA "Temp\.net\ResourceMonitor"
    if (Test-Path $cacheDir) {
        Remove-Item -Recurse -Force $cacheDir -ErrorAction SilentlyContinue
        Write-Host "[$Label] Cache pulita: $cacheDir"
    }
}

Write-Host ""
Write-Host "=== Test: $Label ===" -ForegroundColor Cyan
$exeFile = Get-Item $ExePath
Write-Host "Exe size: $([math]::Round($exeFile.Length / 1MB, 2)) MB"

# DLL deps next to exe
$distDir = Split-Path $ExePath -Parent
$dllSize = (Get-ChildItem $distDir -Filter "*.dll" -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host "Native DLLs: $([math]::Round($dllSize, 2)) MB"

# Start with stopwatch
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $ExePath -PassThru
$startTime = $sw.ElapsedMilliseconds
Write-Host "Process started at: ${startTime}ms"

# WaitForInputIdle: window message loop ready
try { $proc.WaitForInputIdle(15000) | Out-Null } catch {}
$inputIdleMs = $sw.ElapsedMilliseconds
Write-Host "Input idle (msg loop ready): ${inputIdleMs}ms" -ForegroundColor Green

# Try detecting WPF window via FindWindow (title = "Resource Monitor")
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinApi {
    [DllImport("user32.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
}
"@ -ErrorAction SilentlyContinue
$windowMs = -1
$timeout = 20000
while ($sw.ElapsedMilliseconds -lt $timeout) {
    $h = [WinApi]::FindWindow($null, "Resource Monitor")
    if ($h -ne [IntPtr]::Zero) {
        $windowMs = $sw.ElapsedMilliseconds
        break
    }
    Start-Sleep -Milliseconds 30
}
if ($windowMs -ge 0) { Write-Host "Window found via FindWindow: ${windowMs}ms" -ForegroundColor Green }
else { Write-Host "Window never found by title" -ForegroundColor Yellow }

# Wait until WorkingSet stops growing significantly (settled, skip init phase)
Start-Sleep -Seconds 25
$proc.Refresh()
$ramAfter8s_WS = [math]::Round($proc.WorkingSet64 / 1MB, 1)
$ramAfter8s_Priv = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
Write-Host "RAM @ 25s settled: WS=${ramAfter8s_WS} MB, Private=${ramAfter8s_Priv} MB"

# CPU idle sampling
Write-Host "Sampling CPU per ${IdleSampleSeconds}s..."
$proc.Refresh()
$cpu_start = $proc.TotalProcessorTime.TotalSeconds
$wallStart = Get-Date
Start-Sleep -Seconds $IdleSampleSeconds
$proc.Refresh()
$cpu_end = $proc.TotalProcessorTime.TotalSeconds
$wallElapsed = ((Get-Date) - $wallStart).TotalSeconds
$avgCpuPct = (($cpu_end - $cpu_start) / $wallElapsed) * 100
Write-Host ("Avg CPU idle: {0:N2}%" -f $avgCpuPct) -ForegroundColor Yellow

# Final memory
$proc.Refresh()
$finalWS = [math]::Round($proc.WorkingSet64 / 1MB, 1)
$finalPriv = [math]::Round($proc.PrivateMemorySize64 / 1MB, 1)
$threads = $proc.Threads.Count
Write-Host "Final: WS=${finalWS} MB, Private=${finalPriv} MB, Threads=$threads"

# Output result row
$result = [PSCustomObject]@{
    Label = $Label
    ExeMB = [math]::Round($exeFile.Length / 1MB, 2)
    DllMB = [math]::Round($dllSize, 2)
    WindowMs = $windowMs
    RamSettledMB = $ramAfter8s_Priv
    RamFinalMB = $finalPriv
    CpuIdlePct = [math]::Round($avgCpuPct, 2)
    Threads = $threads
}

$result | Format-Table -AutoSize | Out-String | Write-Host

# Append to CSV
$csvPath = "C:\Users\wcast\Desktop\resourcemonitor\perf-history.csv"
if (-not (Test-Path $csvPath)) {
    $result | Export-Csv -Path $csvPath -NoTypeInformation
} else {
    $result | Export-Csv -Path $csvPath -NoTypeInformation -Append
}

Write-Host "Salvato in $csvPath"

return $result
