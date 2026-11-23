# Phase 1 benchmark. Measures the real process; nothing is estimated.
#   Idle      : resident instance, overlay never opened, sampled for -IdleSeconds.
#   Windows N : mc_testwin opens N windows; MissionControl --bench runs -Cycles open/close cycles
#               restricted to those windows (--class MCBenchWindow). Process metrics sampled every ~250 ms.
#   Soak      : -SoakCycles open/close cycles with -SoakWindows windows, while mc_testwin churns windows.
# Output (in -OutDir): frames_<N>.csv (per frame), samples_<label>.csv (per sample), summary.csv.
param(
    [int[]]$Counts = @(5, 10, 20, 30),
    [int]$Cycles = 10,
    [int]$IdleSeconds = 300,
    [int]$SoakCycles = 0,
    [int]$SoakSeconds = 0,   # >0: soak runs for this long instead of a cycle count
    [int]$SoakWindows = 20,
    [string]$Build = "$PSScriptRoot\..\build\Release",
    [string]$OutDir = "$PSScriptRoot\results",
    [string[]]$McArgs = @()  # extra MissionControl flags, e.g. --no-trim for A/B runs
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Build = (Resolve-Path $Build).Path
$cores = [Environment]::ProcessorCount
$summary = @()

function Get-GpuMem([int]$procId) {
    # Raw counters; one sample is enough. Sum over adapters (instance names carry pid + LUID).
    $paths = "\GPU Process Memory(pid_${procId}_*)\Dedicated Usage", "\GPU Process Memory(pid_${procId}_*)\Shared Usage"
    try {
        $s = (Get-Counter -Counter $paths -MaxSamples 1 -ErrorAction Stop).CounterSamples
        [pscustomobject]@{
            Dedicated = ($s | Where-Object Path -like '*dedicated usage' | Measure-Object CookedValue -Sum).Sum
            Shared    = ($s | Where-Object Path -like '*shared usage' | Measure-Object CookedValue -Sum).Sum
        }
    } catch { [pscustomobject]@{ Dedicated = $null; Shared = $null } }
}

# Samples $proc until it exits or $seconds elapse. Returns the sample rows.
function Watch-Process($proc, [double]$seconds, [string]$label) {
    $rows = @()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lastCpu = $proc.TotalProcessorTime.TotalMilliseconds; $lastT = 0.0
    while (-not $proc.HasExited -and $sw.Elapsed.TotalSeconds -lt $seconds) {
        Start-Sleep -Milliseconds 250
        $proc.Refresh()
        if ($proc.HasExited) { break }
        $t = $sw.Elapsed.TotalMilliseconds
        $cpu = $proc.TotalProcessorTime.TotalMilliseconds
        $gpu = Get-GpuMem $proc.Id
        $rows += [pscustomobject]@{
            label = $label; t_ms = [math]::Round($t)
            working_set_mb = [math]::Round($proc.WorkingSet64 / 1MB, 2)
            private_bytes_mb = [math]::Round($proc.PrivateMemorySize64 / 1MB, 2)
            cpu_pct = [math]::Round(100 * ($cpu - $lastCpu) / (($t - $lastT) * $cores), 3)
            gpu_dedicated_mb = if ($null -ne $gpu.Dedicated) { [math]::Round($gpu.Dedicated / 1MB, 2) } else { '' }
            gpu_shared_mb = if ($null -ne $gpu.Shared) { [math]::Round($gpu.Shared / 1MB, 2) } else { '' }
            handles = $proc.HandleCount
        }
        $lastCpu = $cpu; $lastT = $t
    }
    $rows = @($rows | Where-Object { $_.working_set_mb -gt 0 })  # drop a sample taken while the process was exiting
    $rows | Export-Csv -NoTypeInformation "$OutDir\samples_$label.csv"
    $rows
}

function Get-Pct([double[]]$v, [double]$p) {
    if (-not $v.Count) { return '' }
    $s = $v | Sort-Object
    $s[[math]::Min($s.Count - 1, [math]::Floor($p * ($s.Count - 1)))]
}

function Get-FrameStats([string]$csv) {
    $f = Import-Csv $csv
    $iv = [double[]]($f | Where-Object { [double]$_.interval_ms -gt 0 } | ForEach-Object { [double]$_.interval_ms })
    $wk = [double[]]($f | ForEach-Object { [double]$_.work_ms })
    $refresh = [double]$f[0].refresh_ms
    $mean = ($iv | Measure-Object -Average).Average
    # Open latency: hotkey/command -> first entrance frame applied (includes thumbnail registration).
    $lat = [double[]]($f | Where-Object { $_.kind -eq 'enter' -and [double]$_.latency_ms -gt 0 } | ForEach-Object { [double]$_.latency_ms })
    [pscustomobject]@{
        frames = $f.Count; refresh_ms = $refresh
        interval_mean_ms = [math]::Round($mean, 3)
        interval_p95_ms = [math]::Round((Get-Pct $iv 0.95), 3)
        interval_p99_ms = [math]::Round((Get-Pct $iv 0.99), 3)
        interval_max_ms = [math]::Round(($iv | Measure-Object -Maximum).Maximum, 3)
        fps_mean = [math]::Round(1000 / $mean, 1)
        long_frames = @($iv | Where-Object { $_ -gt 1.5 * $refresh }).Count
        work_mean_ms = [math]::Round(($wk | Measure-Object -Average).Average, 4)
        work_max_ms = [math]::Round(($wk | Measure-Object -Maximum).Maximum, 4)
        open_latency_mean_ms = if ($lat.Count) { [math]::Round(($lat | Measure-Object -Average).Average, 2) } else { '' }
        open_latency_max_ms = if ($lat.Count) { [math]::Round(($lat | Measure-Object -Maximum).Maximum, 2) } else { '' }
    }
}

function Add-Summary($label, $windows, $rows, $frames) {
    $script:summary += [pscustomobject]@{
        scenario = $label; windows = $windows
        ws_peak_mb = ($rows | Measure-Object working_set_mb -Maximum).Maximum
        ws_end_mb = $rows[-1].working_set_mb
        private_peak_mb = ($rows | Measure-Object private_bytes_mb -Maximum).Maximum
        private_end_mb = $rows[-1].private_bytes_mb
        cpu_avg_pct = [math]::Round(($rows | Measure-Object cpu_pct -Average).Average, 3)
        cpu_max_pct = ($rows | Measure-Object cpu_pct -Maximum).Maximum
        gpu_dedicated_peak_mb = ($rows | Where-Object gpu_dedicated_mb -ne '' | Measure-Object gpu_dedicated_mb -Maximum).Maximum
        gpu_shared_peak_mb = ($rows | Where-Object gpu_shared_mb -ne '' | Measure-Object gpu_shared_mb -Maximum).Maximum
        handles_end = $rows[-1].handles
        frames = $frames.frames; refresh_ms = $frames.refresh_ms; fps_mean = $frames.fps_mean
        interval_mean_ms = $frames.interval_mean_ms; interval_p95_ms = $frames.interval_p95_ms
        interval_p99_ms = $frames.interval_p99_ms; interval_max_ms = $frames.interval_max_ms
        long_frames = $frames.long_frames; work_mean_ms = $frames.work_mean_ms; work_max_ms = $frames.work_max_ms
        open_latency_mean_ms = $frames.open_latency_mean_ms; open_latency_max_ms = $frames.open_latency_max_ms
    }
}

$mc = "$Build\MissionControl.exe"; $tw = "$Build\mc_testwin.exe"

if ($IdleSeconds -gt 0) {
    Write-Host "idle: $IdleSeconds s"
    $p = if ($McArgs.Count) { Start-Process $mc -ArgumentList $McArgs -PassThru } else { Start-Process $mc -PassThru }
    Start-Sleep -Seconds 2  # let startup settle
    $rows = Watch-Process $p $IdleSeconds 'idle'
    Start-Process $mc -ArgumentList '--quit' -Wait
    if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id; Write-Warning 'idle instance did not quit' }
    Add-Summary 'idle' 0 $rows ([pscustomobject]@{})
}

foreach ($n in $Counts) {
    Write-Host "windows: $n x $Cycles cycles"
    $t = Start-Process $tw -ArgumentList $n -PassThru
    Start-Sleep -Milliseconds 1500
    $csv = "$OutDir\frames_$n.csv"
    $p = Start-Process $mc -ArgumentList (@('--bench', $Cycles, '--csv', $csv, '--class', 'MCBenchWindow') + $McArgs) -PassThru
    $rows = Watch-Process $p 600 "windows_$n"
    $p.WaitForExit()
    Stop-Process -Id $t.Id -ErrorAction SilentlyContinue
    Add-Summary "windows_$n" $n $rows (Get-FrameStats $csv)
}

if ($SoakSeconds -gt 0) { $SoakCycles = 1000000 }
if ($SoakCycles -gt 0) {
    Write-Host "soak: $SoakWindows windows (churning every 700 ms) x $SoakCycles cycles"
    $t = Start-Process $tw -ArgumentList $SoakWindows, '--churn', 700 -PassThru
    Start-Sleep -Milliseconds 1500
    $csv = "$OutDir\frames_soak.csv"
    $timeArgs = if ($SoakSeconds -gt 0) { @('--bench-seconds', $SoakSeconds) } else { @() }
    $p = Start-Process $mc -ArgumentList (@('--bench', $SoakCycles, '--csv', $csv, '--class', 'MCBenchWindow') + $timeArgs + $McArgs) -PassThru
    $rows = Watch-Process $p ([math]::Max(7200, $SoakSeconds + 600)) 'soak'
    $p.WaitForExit()
    Stop-Process -Id $t.Id -ErrorAction SilentlyContinue
    Add-Summary 'soak' $SoakWindows $rows (Get-FrameStats $csv)
}

$summary | Export-Csv -NoTypeInformation "$OutDir\summary.csv"
$summary | Format-Table scenario, ws_peak_mb, private_peak_mb, cpu_avg_pct, gpu_dedicated_peak_mb, gpu_shared_peak_mb, fps_mean, interval_p99_ms, long_frames, work_max_ms -AutoSize
