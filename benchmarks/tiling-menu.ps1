# Measures the cost of the traffic-light overlay and the tiling menu on a disposable window (mc_testwin).
# Starts its own MissionControl (--no-takeover, config that opts mc_testwin in) and stops it again. Moves the mouse:
# do not use the machine while it runs (about 2 minutes).
#   benchmarks\tiling-menu.ps1 [-Build build\Release]
param([string]$Build = (Join-Path $PSScriptRoot '..\build\Release'))
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Build 'MissionControl.exe'; $testwin = Join-Path $Build 'mc_testwin.exe'
$cfg = Join-Path $env:TEMP 'mc-tiling-bench.json'
Set-Content $cfg '{"globalTrafficLights":{"experimentalOverlay":{"enabled":true,"apps":{"mc_testwin.exe":{"enabled":true}}}}}'
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class B {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;} public delegate bool EP(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v); [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f,int x,int y,uint d,IntPtr e); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n); [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EP p, IntPtr l); [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c); [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
 [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr p, uint f); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, IntPtr ex);
 public static void Mv(int x,int y){ int vx=GetSystemMetrics(76), vy=GetSystemMetrics(77), vw=GetSystemMetrics(78), vh=GetSystemMetrics(79); mouse_event(0xC001, (int)(((x-vx)*65535L)/(vw-1)), (int)(((y-vy)*65535L)/(vh-1)), 0, IntPtr.Zero); }
 public static string Cls(IntPtr h){ var c=new StringBuilder(64); GetClassName(h,c,64); return c.ToString(); }
 public static IntPtr Find(string cls, IntPtr owner){ IntPtr f=IntPtr.Zero; EnumWindows((h,l)=>{ if(Cls(h)==cls && (owner==IntPtr.Zero || GetWindow(h,4)==owner)){ f=h; return false;} return true;}, IntPtr.Zero); return f; }
}
'@
[B]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null
[B]::Mv(10, 700); Start-Sleep -Milliseconds 600
$mc = Start-Process $exe -ArgumentList '--no-takeover', '--config', $cfg -PassThru; Start-Sleep 2
$tw = Start-Process $testwin -ArgumentList '1' -PassThru; Start-Sleep 2
$h = $tw.MainWindowHandle
foreach ($i in 1..3) { [B]::keybd_event(0x7E,0,0,[IntPtr]::Zero); [B]::keybd_event(0x7E,0,2,[IntPtr]::Zero); [B]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 250 }
Start-Sleep 1
$ov = [B]::Find('MCEdgeTrafficLightsPoc', $h); $r = New-Object B+RECT; [B]::GetWindowRect($ov, [ref]$r) | Out-Null
$cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2)   # centre = yellow dot; red = -20, green = +20
$cores = [Environment]::ProcessorCount
function Snap { $mc.Refresh(); [pscustomobject]@{ T = Get-Date; Cpu = $mc.TotalProcessorTime.TotalSeconds; WS = $mc.WorkingSet64 / 1MB; Priv = $mc.PrivateMemorySize64 / 1MB; Handles = $mc.HandleCount; Gdi = [B]::GetGuiResources($mc.Handle, 0); User = [B]::GetGuiResources($mc.Handle, 1); Threads = $mc.Threads.Count } }
function Cpu($a, $b) { [math]::Round(($b.Cpu - $a.Cpu) / (($b.T - $a.T).TotalSeconds) / $cores * 100, 3) }
function Row($name, $a, $b) { "{0,-34} cpu {1,6}%   ws {2,6:N1} MB  private {3,6:N1} MB  handles {4}  gdi {5}  user {6}  threads {7}" -f $name, (Cpu $a $b), $b.WS, $b.Priv, $b.Handles, $b.Gdi, $b.User, $b.Threads }
$out = @()
$out += "overlays: " + @(Get-Process mc_testwin).Count + " test window; logical CPUs: $cores"
# 1. idle (overlay attached, mouse elsewhere)
$a = Snap; Start-Sleep 10; $b = Snap; $out += Row 'idle 10 s' $a $b
# 2. hover: the mouse sweeps over the three dots without stopping on the green one (no menu)
$a = Snap; $end = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $end) { foreach ($dx in -30, -10, 10, 30, 10, -10) { [B]::Mv($cx + $dx, $cy); Start-Sleep -Milliseconds 60 } }
[B]::Mv(10, 700); Start-Sleep -Milliseconds 500; $b = Snap; $out += Row 'hover sweep 15 s (no menu)' $a $b
# 3. menu open / close cycles
function Cycles($n) { foreach ($i in 1..$n) { [B]::Mv($cx + 17, $cy); Start-Sleep -Milliseconds 40; [B]::Mv($cx + 20, $cy); Start-Sleep -Milliseconds 600; [B]::Mv(10, 700); Start-Sleep -Milliseconds 450 } }
Cycles 5; $warm = Snap
$a = Snap; Cycles 60; $b = Snap; $out += Row 'menu open/close x60' $a $b
$out += "   memory over those 60 cycles: private {0:+0.0;-0.0} MB, ws {1:+0.0;-0.0} MB, handles {2:+0;-0;0}, gdi {3:+0;-0;0}, user {4:+0;-0;0}" -f ($b.Priv - $warm.Priv), ($b.WS - $warm.WS), ($b.Handles - $warm.Handles), ($b.Gdi - $warm.Gdi), ($b.User - $warm.User)
$mid = Snap; Cycles 60; $end2 = Snap
$out += "   next 60 cycles (plateau check): private {0:+0.0;-0.0} MB, ws {1:+0.0;-0.0} MB, handles {2:+0;-0;0}, gdi {3:+0;-0;0}, user {4:+0;-0;0}" -f ($end2.Priv - $mid.Priv), ($end2.WS - $mid.WS), ($end2.Handles - $mid.Handles), ($end2.Gdi - $mid.Gdi), ($end2.User - $mid.User)
# 4. latency: click on the green dot -> the window is maximized; and hover -> menu visible (the delay is 350 ms by design)
$lat = @()
foreach ($i in 1..8) {
  [B]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 1200   # restored; re-read where the strip is
  [B]::GetWindowRect([B]::Find('MCEdgeTrafficLightsPoc', $h), [ref]$r) | Out-Null; $cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2)
  [B]::Mv($cx + 17, $cy); Start-Sleep -Milliseconds 100; [B]::Mv($cx + 20, $cy); Start-Sleep -Milliseconds 700   # menu is open now
  $t0 = Get-Date; [B]::mouse_event(2,0,0,0,[IntPtr]::Zero); [B]::mouse_event(4,0,0,0,[IntPtr]::Zero)
  while (-not [B]::IsZoomed($h) -and ((Get-Date) - $t0).TotalMilliseconds -lt 2000) { Start-Sleep -Milliseconds 1 }
  $lat += [int]((Get-Date) - $t0).TotalMilliseconds
  [B]::Mv(10, 700); Start-Sleep -Milliseconds 500
}
$out += "click on green -> window maximized (menu open): ms " + ($lat -join ', ') + "  median " + ($lat | Sort-Object)[[int]($lat.Count / 2)]
$out
Stop-Process -Id $tw.Id, $mc.Id -Force -ErrorAction SilentlyContinue
