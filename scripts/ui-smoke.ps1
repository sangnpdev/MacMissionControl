# UI smoke test: hover, keyboard navigation and the scroll gesture, driven by messages posted to the
# overlay window only (never global input injection). Uses mc_testwin windows; screenshots optional.
# Usage: scripts\ui-smoke.ps1 [-Shots DIR]
param([string]$Build = "$PSScriptRoot\..\build\Release", [string]$Shots = '')
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Ov {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public static IntPtr XY(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
}
'@
[Ov]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null  # physical pixels, like the app
function Shot([string]$name) { if ($Shots) { & "$PSScriptRoot\screenshot.ps1" -Out (Join-Path $Shots "$name.png") } }
function Log { Get-Content "$env:LOCALAPPDATA\MissionControl\mc.log" }

$mc = "$Build\MissionControl.exe"
$tw = Start-Process "$Build\mc_testwin.exe" -ArgumentList 4 -PassThru
try {
    Start-Sleep -Milliseconds 1500
    $app = Start-Process $mc -ArgumentList '--show', '--class', 'MCBenchWindow' -PassThru
    Start-Sleep -Milliseconds 1500
    $ov = [Ov]::FindWindow('MissionControlOverlay', [NullString]::Value)  # $null would become "" and match nothing
    $r = New-Object Ov+RECT; [Ov]::GetWindowRect($ov, [ref]$r) | Out-Null
    # Client point for a screen point on the primary monitor (client origin = virtual-screen origin).
    function Client([int]$sx, [int]$sy) { [Ov]::XY($sx - $r.Left, $sy - $r.Top) }

    # Hover: sweep the pointer over the primary screen; the card under it gets the focus ring.
    foreach ($x in 300, 700, 1100) { [Ov]::PostMessage($ov, 0x0200, [IntPtr]0, (Client $x 600)) | Out-Null; Start-Sleep -Milliseconds 120 }
    Start-Sleep -Milliseconds 400; Shot '1-hover'
    # Keyboard: Right, then Down.
    [Ov]::PostMessage($ov, 0x0100, [IntPtr]0x27, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 400; Shot '2-key-right'
    [Ov]::PostMessage($ov, 0x0100, [IntPtr]0x28, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 400; Shot '3-key-down'

    # Small, slow scroll then stop: must spring back and stay open.
    foreach ($i in 1..2) { [Ov]::PostMessage($ov, 0x020A, [IntPtr](40 -shl 16), (Client 960 540)) | Out-Null; Start-Sleep -Milliseconds 60 }
    Start-Sleep -Milliseconds 50; Shot '4-scrub-small'
    Start-Sleep -Milliseconds 900
    $stayed = (Log | Select-String 'gesture release' | Select-Object -Last 1) -match 'stay'
    Write-Host "slow small scroll -> stayed open: $stayed"

    # Fast scroll: must close with the gesture spring.
    foreach ($i in 1..4) { [Ov]::PostMessage($ov, 0x020A, [IntPtr](120 -shl 16), (Client 960 540)) | Out-Null; Start-Sleep -Milliseconds 16 }
    Shot '5-scrub-fast'
    Start-Sleep -Milliseconds 1200
    $closed = (Log | Select-String 'close: gesture').Count -gt 0
    Write-Host "fast scroll -> closed by gesture: $closed"
    if (-not ($stayed -and $closed)) { Write-Warning 'gesture behaviour not as expected' }
} finally {
    Start-Process $mc -ArgumentList '--quit' -Wait
    if (-not $tw.HasExited) { Stop-Process -Id $tw.Id }
}
Log | Select-String 'gesture|close:|transition'
