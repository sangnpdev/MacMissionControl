# Saves a half-size screenshot in physical pixels. Usage: screenshot.ps1 -Out file.png [-Virtual]
#   -Virtual  capture every monitor (the whole virtual screen) instead of the primary one
param([Parameter(Mandatory)][string]$Out, [switch]$Virtual)
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v); }
'@
[Dpi]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null  # PER_MONITOR_AWARE_V2
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$b = if ($Virtual) { [System.Windows.Forms.SystemInformation]::VirtualScreen } else { [System.Windows.Forms.Screen]::PrimaryScreen.Bounds }
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($b.Width / 2)), ([int]($b.Height / 2))
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $small.Dispose()
