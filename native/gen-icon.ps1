$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$out = Join-Path $PSScriptRoot 'app.ico'
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$pngs = @()

foreach ($s in $sizes) {
  $bmp = New-Object System.Drawing.Bitmap($s, $s)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.Clear([System.Drawing.Color]::Transparent)

  $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
  $round = New-Object System.Drawing.Drawing2D.GraphicsPath
  $r = [Math]::Max(2, [int]($s * 0.22))
  $round.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
  $round.AddArc($s - $r * 2, 0, $r * 2, $r * 2, 270, 90)
  $round.AddArc($s - $r * 2, $s - $r * 2, $r * 2, $r * 2, 0, 90)
  $round.AddArc(0, $s - $r * 2, $r * 2, $r * 2, 90, 90)
  $round.CloseFigure()

  $c1 = [System.Drawing.Color]::FromArgb(255, 47, 107, 255)
  $c2 = [System.Drawing.Color]::FromArgb(255, 96, 165, 250)
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45)
  $g.FillPath($brush, $round)

  # wifi arcs + dot
  $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [Math]::Max(1.1, $s * 0.075))
  $pen.StartCap = 'Round'
  $pen.EndCap = 'Round'
  $cx = $s / 2.0
  $cy = $s * 0.72
  foreach ($k in 1..3) {
    $rad = $s * 0.16 * $k
    $g.DrawArc($pen, $cx - $rad, $cy - $rad, $rad * 2, $rad * 2, 205, 130)
  }
  $dot = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
  $d = [Math]::Max(1.6, $s * 0.085)
  $g.FillEllipse($dot, $cx - $d / 2, $cy - $d / 2, $d, $d)

  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $pngs += , $ms.ToArray()
  $ms.Dispose()
  $bmp.Dispose()
  $brush.Dispose(); $pen.Dispose(); $dot.Dispose(); $round.Dispose()
}

# write ICO container (PNG-compressed entries, Vista+)
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)
$bw.Write([UInt16]1)
$bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]
  $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
  $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
  $bw.Write([Byte]0)
  $bw.Write([Byte]0)
  $bw.Write([UInt16]1)
  $bw.Write([UInt16]32)
  $bw.Write([UInt32]$pngs[$i].Length)
  $bw.Write([UInt32]$offset)
  $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close(); $fs.Close()
Write-Host "app.ico: $([math]::Round((Get-Item $out).Length/1KB,1)) KB, $($sizes.Count) sizes"
