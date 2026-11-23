# Stability stress test (Phase 6). Messages are posted to the overlay window only; the desktop part
# creates ONE desktop and removes only that one at the end (same safeguards as desktop-smoke.ps1).
#   1. 200 open/close toggles at random 0-250 ms intervals (interrupting transitions)
#   2. simulated display change and resume-from-sleep, while open and while idle
#   3. 30 rapid desktop switches while the overview is open
# Usage: scripts\stress.ps1 [-Toggles 200] [-Switches 30] [-SkipDesktops]
param([string]$Build = "$PSScriptRoot\..\build\Release", [int]$Toggles = 200, [int]$Switches = 30, [switch]$SkipDesktops)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class S {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void CtrlWin(byte key, bool ext) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero); keybd_event(0x5B, 0, 1, UIntPtr.Zero);
        keybd_event(key, 0, ext ? 1u : 0u, UIntPtr.Zero); keybd_event(key, 0, (ext ? 1u : 0u) | 2, UIntPtr.Zero);
        keybd_event(0x5B, 0, 3, UIntPtr.Zero); keybd_event(0x11, 0, 2, UIntPtr.Zero);
    }
}
'@
$WM_HOTKEY = 0x0312; $WM_DISPLAYCHANGE = 0x007E; $WM_POWERBROADCAST = 0x0218; $PBT_RESUME = 0x12
function Log { Get-Content "$env:LOCALAPPDATA\MissionControl\mc.log" }
function Snap($p, $label) { $p.Refresh(); [pscustomobject]@{ step = $label; alive = -not $p.HasExited; handles = $p.HandleCount
    private_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 1); ws_mb = [math]::Round($p.WorkingSet64 / 1MB, 1) } }
$mc = "$Build\MissionControl.exe"
$rows = @()
$tw = Start-Process "$Build\mc_testwin.exe" -ArgumentList 10 -PassThru
$app = $null; $created = $null
try {
    Start-Sleep -Milliseconds 1500
    $app = Start-Process $mc -ArgumentList '--class', 'MCBenchWindow', '--no-takeover' -PassThru
    Start-Sleep -Milliseconds 2000
    $ov = [S]::FindWindow('MissionControlOverlay', [NullString]::Value)
    $rows += Snap $app 'start (idle)'

    # 1. rapid toggles
    $rnd = New-Object Random 42
    for ($i = 0; $i -lt $Toggles; $i++) { [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds $rnd.Next(0, 250) }
    if ([S]::IsWindowVisible($ov)) { [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null }
    Start-Sleep -Milliseconds 1500
    $rows += Snap $app "after $Toggles toggles"

    # 2. display change / resume, open and idle
    [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 300
    [S]::PostMessage($ov, $WM_DISPLAYCHANGE, [IntPtr]32, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1500
    [S]::PostMessage($ov, $WM_POWERBROADCAST, [IntPtr]$PBT_RESUME, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1500
    [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 300
    [S]::PostMessage($ov, $WM_POWERBROADCAST, [IntPtr]$PBT_RESUME, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1500
    [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1200   # must still open
    $reopened = [S]::IsWindowVisible($ov)
    [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1000
    $rows += Snap $app 'after display/resume events'

    # 3. rapid desktop switching inside the overview
    if (-not $SkipDesktops) {
        $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops'
        $ids = { $raw = (Get-ItemProperty $key).VirtualDesktopIDs; 0..($raw.Length / 16 - 1) | ForEach-Object { [BitConverter]::ToString($raw, $_ * 16, 16) } }
        $cur = { [BitConverter]::ToString((Get-ItemProperty $key).CurrentVirtualDesktop) }
        $startIds = @(& $ids); $home_ = & $cur
        [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1000
        Start-Process $mc -ArgumentList '--desktop', 'new' -Wait; Start-Sleep -Milliseconds 1200
        $created = & $cur
        if ($startIds -contains $created) { $created = $null; throw 'desktop was not created' }
        for ($i = 0; $i -lt $Switches; $i++) {
            Start-Process $mc -ArgumentList '--desktop', ($(if ($i % 2) { 'next' } else { 'prev' })) -Wait
            Start-Sleep -Milliseconds (Get-Random -Minimum 80 -Maximum 400)
        }
        Start-Sleep -Milliseconds 1500
        [S]::PostMessage($ov, $WM_HOTKEY, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 1000
        $rows += Snap $app "after $Switches desktop switches"
    }
} finally {
    if ($app -and -not $app.HasExited) { Start-Process $mc -ArgumentList '--quit' -Wait; Start-Sleep -Milliseconds 800 }
    if (-not $tw.HasExited) { Stop-Process -Id $tw.Id }
    if ($created) {
        # Remove only the created desktop: walk to it, check its id, Ctrl+Win+F4, walk home.
        for ($g = 0; $g -lt 10 -and (& $cur) -ne $created; $g++) {
            $all = @(& $ids); $right = [array]::IndexOf($all, $created) -gt [array]::IndexOf($all, (& $cur))
            [S]::CtrlWin($(if ($right) { 0x27 } else { 0x25 }), $true); Start-Sleep -Milliseconds 600
        }
        if ((& $cur) -eq $created) { [S]::CtrlWin(0x73, $false); Start-Sleep -Milliseconds 1000 } else { Write-Warning 'created desktop not removed; remove it by hand' }
        for ($g = 0; $g -lt 10 -and (& $cur) -ne $home_; $g++) {
            $all = @(& $ids); $right = [array]::IndexOf($all, $home_) -gt [array]::IndexOf($all, (& $cur))
            [S]::CtrlWin($(if ($right) { 0x27 } else { 0x25 }), $true); Start-Sleep -Milliseconds 600
        }
        $restored = ((@(& $ids)) -join ',') -eq ($startIds -join ',') -and (& $cur) -eq $home_
        Write-Host "desktops restored: $restored"
    }
}
$rows | Format-Table -AutoSize
Write-Host "reopened after display/resume: $reopened"
Write-Host "process exited cleanly: $($app.HasExited)"
$errs = Log | Select-String 'ERROR|CRASH'
Write-Host "ERROR/CRASH lines in log: $(@($errs).Count)"; $errs | Select-Object -First 10
Log | Select-String 'display change|resume|rebuild|scene renderer ready' | Select-Object -First 12
