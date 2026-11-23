# Live test of the Win+Tab takeover (Phase 5). Injects Win+Tab on the real desktop:
#   1. app running (hook in test mode, which accepts synthetic keys): Win+Tab must open the overview,
#      and neither Start nor Task View may appear; Win+Tab again closes it.
#   2. app killed (like a crash): Win+Tab must open Windows Task View again (closed with Esc).
# Usage: scripts\takeover-smoke.ps1 [-Shots DIR]
param([string]$Build = "$PSScriptRoot\..\build\Release", [string]$Shots = '')
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class K {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    const uint UP = 2, EXT = 1;
    public static void WinTab() {
        keybd_event(0x5B, 0, EXT, UIntPtr.Zero);
        keybd_event(0x09, 0, 0, UIntPtr.Zero);
        keybd_event(0x09, 0, UP, UIntPtr.Zero);
        keybd_event(0x5B, 0, EXT | UP, UIntPtr.Zero);
    }
    public static void Esc() { keybd_event(0x1B, 0, 0, UIntPtr.Zero); keybd_event(0x1B, 0, UP, UIntPtr.Zero); }
    public static string Foreground() {
        IntPtr h = GetForegroundWindow(); var c = new StringBuilder(256); var t = new StringBuilder(256);
        GetClassName(h, c, 256); GetWindowText(h, t, 256);
        return c + " | " + t;
    }
}
'@
function Shot([string]$name) { if ($Shots) { & "$PSScriptRoot\screenshot.ps1" -Out (Join-Path $Shots "$name.png") } }
function Log { Get-Content "$env:LOCALAPPDATA\MissionControl\mc.log" }
$mc = "$Build\MissionControl.exe"
$results = [ordered]@{}

# 1. Takeover
$app = Start-Process $mc -ArgumentList '--hook-accept-injected' -PassThru
Start-Sleep -Milliseconds 2500   # hook installed, backdrops prepared
[K]::WinTab()
Start-Sleep -Milliseconds 900
$fg = [K]::Foreground(); Shot 'takeover-1-open'
$results['open by Win+Tab'] = [bool](Log | Select-String 'open: win\+tab')
$results['foreground after Win+Tab'] = $fg
$results['Start/Task View not shown'] = -not ($fg -match 'Start|Task View|XamlExplorerHost|MultitaskingView')
[K]::WinTab()
Start-Sleep -Milliseconds 900
Shot 'takeover-2-closed'
$results['closed by Win+Tab'] = [bool](Log | Select-String 'close: win\+tab')
$results['foreground after close'] = [K]::Foreground()

# 2. Crash safety: kill the process the hard way, then Win+Tab must reach Windows again.
Stop-Process -Id $app.Id -Force
Start-Sleep -Milliseconds 800
[K]::WinTab()
Start-Sleep -Milliseconds 1500
$fg = [K]::Foreground(); Shot 'takeover-3-after-kill'
$results['foreground after kill + Win+Tab'] = $fg
[K]::Esc()   # close Task View
Start-Sleep -Milliseconds 800
$results['foreground after Esc'] = [K]::Foreground()

$results.GetEnumerator() | ForEach-Object { "{0,-34} {1}" -f $_.Key, $_.Value }
