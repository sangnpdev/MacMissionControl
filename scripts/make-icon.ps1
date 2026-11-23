# Builds resources\app.ico from a square-ish PNG with transparency: crops a square around the
# opaque tile (dropping stray pixels outside it), resizes with high-quality bicubic, and writes a
# multi-size ICO with PNG-compressed images (supported since Windows Vista).
# Usage: scripts\make-icon.ps1 [-Source resources\app-icon.png] [-Out resources\app.ico]
param([string]$Source = "$PSScriptRoot\..\resources\app-icon.png", [string]$Out = "$PSScriptRoot\..\resources\app.ico",
      [int[]]$Sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256), [double]$Margin = 0.03)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Source))
try {
    # Opaque tile bounds: rows/columns with at least 50 strongly opaque samples (sparse noise does not count).
    $W = $src.Width; $H = $src.Height; $rows = New-Object int[] $H; $cols = New-Object int[] $W
    for ($y = 0; $y -lt $H; $y += 2) { for ($x = 0; $x -lt $W; $x += 2) { if ($src.GetPixel($x, $y).A -gt 200) { $rows[$y]++; $cols[$x]++ } } }
    $top = 0; while ($rows[$top] -lt 50) { $top++ }; $bot = $H - 1; while ($rows[$bot] -lt 50) { $bot-- }
    $left = 0; while ($cols[$left] -lt 50) { $left++ }; $right = $W - 1; while ($cols[$right] -lt 50) { $right-- }
    # Clear stray pixels outside the tile (plus a small band kept for its glow).
    $pad = [int](($right - $left) * 0.006)
    $clear = [System.Drawing.Color]::FromArgb(0, 0, 0, 0)
    for ($y = 0; $y -lt $H; $y++) {
        $inRows = $y -ge $top - $pad -and $y -le $bot + $pad
        for ($x = 0; $x -lt $W; $x++) {
            if ($inRows -and $x -ge $left - $pad -and $x -le $right + $pad) { $x = $right + $pad; continue }
            if ($src.GetPixel($x, $y).A -ne 0) { $src.SetPixel($x, $y, $clear) }
        }
    }
    $side = [int]([math]::Max($right - $left, $bot - $top) * (1 + 2 * $Margin))
    $cx = ($left + $right) / 2; $cy = ($top + $bot) / 2
    $crop = New-Object System.Drawing.Rectangle ([int]($cx - $side / 2)), ([int]($cy - $side / 2)), $side, $side
    Write-Host "tile x $left..$right y $top..$bot -> square crop $($crop.X),$($crop.Y) ${side}px"

    $pngs = @()
    foreach ($s in $Sizes) {
        $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CompositingMode = 'SourceCopy'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'; $g.SmoothingMode = 'HighQuality'
        $attrs = New-Object System.Drawing.Imaging.ImageAttributes
        $attrs.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)  # no dark fringe at the edges
        $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $s, $s), $crop.X, $crop.Y, $crop.Width, $crop.Height, 'Pixel', $attrs)
        $g.Dispose()
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        $pngs += , @($s, $ms.ToArray())
    }

    # ICONDIR + ICONDIRENTRY[] + PNG blobs.
    $fs = [System.IO.File]::Create((Join-Path (Resolve-Path (Split-Path $Out)) (Split-Path $Out -Leaf)))
    $w = New-Object System.IO.BinaryWriter $fs
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$pngs.Count)
    $offset = 6 + 16 * $pngs.Count
    foreach ($p in $pngs) {
        $dim = if ($p[0] -ge 256) { 0 } else { $p[0] }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$p[1].Length); $w.Write([uint32]$offset)
        $offset += $p[1].Length
    }
    foreach ($p in $pngs) { $w.Write($p[1]) }
    $w.Close()
    Write-Host "wrote $Out ($($Sizes -join ', ') px)"
} finally { $src.Dispose() }
