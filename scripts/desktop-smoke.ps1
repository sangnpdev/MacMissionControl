# Live smoke test of desktop switching inside the overview. CHANGES the session for ~10 s:
# creates one virtual desktop, switches between it and the original, then removes ONLY that desktop
# (after checking its id) and verifies the registry matches the starting state.
# Usage: scripts\desktop-smoke.ps1 [-Shots DIR]   (screenshots of the primary screen go to DIR)
param([string]$Build = "$PSScriptRoot\..\build\Release", [string]$Shots = '')
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops'
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class Kb {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
    public static void Chord(byte key, bool extended) {
        const uint UP = 2, EXT = 1;
        keybd_event(0x11, 0, 0, System.UIntPtr.Zero);          // Ctrl
        keybd_event(0x5B, 0, EXT, System.UIntPtr.Zero);        // LWin
        keybd_event(key, 0, extended ? EXT : 0, System.UIntPtr.Zero);
        keybd_event(key, 0, (extended ? EXT : 0) | UP, System.UIntPtr.Zero);
        keybd_event(0x5B, 0, EXT | UP, System.UIntPtr.Zero);
        keybd_event(0x11, 0, UP, System.UIntPtr.Zero);
    }
}
'@

function Get-Desktops {
    $p = Get-ItemProperty $key
    $ids = @(); $raw = $p.VirtualDesktopIDs
    for ($i = 0; $i -lt $raw.Length; $i += 16) { $ids += [BitConverter]::ToString($raw, $i, 16) }
    [pscustomobject]@{ Ids = $ids; Current = [BitConverter]::ToString($p.CurrentVirtualDesktop) }
}
function Wait-Desktop([scriptblock]$cond, [int]$ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) { $d = Get-Desktops; if (& $cond $d) { return $d }; Start-Sleep -Milliseconds 50 }
    throw "timeout waiting for desktop state"
}
function Shot([string]$name) { if ($Shots) { & "$PSScriptRoot\screenshot.ps1" -Out (Join-Path $Shots "$name.png") } }

$start = Get-Desktops
$home_ = $start.Current
Write-Host "start: $($start.Ids.Count) desktop(s)"
$mc = "$Build\MissionControl.exe"; $procs = @()
try {
    $procs += Start-Process "$Build\mc_testwin.exe" -ArgumentList 3 -PassThru
    Start-Sleep -Milliseconds 1500
    $app = Start-Process $mc -ArgumentList '--show', '--class', 'MCBenchWindow' -PassThru
    Start-Sleep -Milliseconds 1500
    Shot '1-open'

    Start-Process $mc -ArgumentList '--desktop', 'new' -Wait
    Start-Sleep -Milliseconds 150; Shot '2-sliding-to-new'
    $d = Wait-Desktop { param($d) $d.Ids.Count -eq $start.Ids.Count + 1 -and $start.Ids -notcontains $d.Current }
    $created = $d.Current
    Start-Sleep -Milliseconds 1000; Shot '3-new-empty'

    $procs += Start-Process "$Build\mc_testwin.exe" -ArgumentList 2 -PassThru   # opens on the new desktop
    Start-Sleep -Milliseconds 1500; Shot '4-new-with-windows'

    Start-Process $mc -ArgumentList '--desktop', ([array]::IndexOf($d.Ids, $home_) + 1) -Wait
    Start-Sleep -Milliseconds 150; Shot '5-sliding-home'
    Wait-Desktop { param($d) $d.Current -eq $home_ } | Out-Null
    Start-Sleep -Milliseconds 1000; Shot '6-home'
    Write-Host 'overview switched to the new desktop and back'
} finally {
    Start-Process $mc -ArgumentList '--quit' -Wait
    $procs | ForEach-Object { if (-not $_.HasExited) { Stop-Process -Id $_.Id } }
    Start-Sleep -Milliseconds 500
    # Remove only the desktop this script created.
    if ($created) {
        for ($guard = 0; $guard -lt 10 -and (Get-Desktops).Current -ne $created; $guard++) {
            $d = Get-Desktops
            $right = [array]::IndexOf($d.Ids, $created) -gt [array]::IndexOf($d.Ids, $d.Current)
            [Kb]::Chord($(if ($right) { 0x27 } else { 0x25 }), $true)
            Start-Sleep -Milliseconds 600
        }
        if ((Get-Desktops).Current -eq $created) {
            [Kb]::Chord(0x73, $false)   # Ctrl+Win+F4 closes the current (created) desktop
            Wait-Desktop { param($d) $d.Ids -notcontains $created } | Out-Null
        } else { Write-Warning "could not reach the created desktop; remove it by hand" }
    }
    for ($guard = 0; $guard -lt 10 -and (Get-Desktops).Current -ne $home_; $guard++) {
        $d = Get-Desktops
        $right = [array]::IndexOf($d.Ids, $home_) -gt [array]::IndexOf($d.Ids, $d.Current)
        [Kb]::Chord($(if ($right) { 0x27 } else { 0x25 }), $true)
        Start-Sleep -Milliseconds 600
    }
    $end = Get-Desktops
    $same = ($end.Ids -join ',') -eq ($start.Ids -join ',') -and $end.Current -eq $home_
    Write-Host "end: $($end.Ids.Count) desktop(s), state restored: $same"
    if (-not $same) { Write-Warning 'desktop state differs from the start' }
}
