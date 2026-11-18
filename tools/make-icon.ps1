# Renders res\PowerMenu.ico (PNG-compressed entries) from the Segoe Fluent power glyph.
param([string]$Out = (Join-Path $PSScriptRoot '..\res\PowerMenu.ico'))
Add-Type -AssemblyName System.Drawing

$glyph = [string][char]0xE7E8
$font = 'Segoe Fluent Icons'
if (-not ([System.Drawing.FontFamily]::Families | Where-Object Name -eq $font)) { $font = 'Segoe MDL2 Assets' }

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded square, red-to-orange gradient.
    $r = [Math]::Max(2, $s * 0.22); $d = 2 * $r
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, $d, $d, 180, 90); $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $w - $d, $d, $d, 0, 90); $path.AddArc(0, $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 232, 72, 56)), ([System.Drawing.Color]::FromArgb(255, 176, 28, 46)), 90
    $g.FillPath($brush, $path)

    $f = New-Object System.Drawing.Font $font, ([float]($s * 0.56)), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
    $g.DrawString($glyph, $f, [System.Drawing.Brushes]::White, $rect, $fmt)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$fs = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$len); $bw.Write([uint32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
[System.IO.File]::WriteAllBytes((Resolve-Path (Split-Path $Out)).Path + '\' + (Split-Path $Out -Leaf), $fs.ToArray())
"wrote $Out"
