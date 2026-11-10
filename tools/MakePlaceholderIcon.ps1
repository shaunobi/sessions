# Draws the interim Sessions icon: three stacked teal cards. The planned
# Blender render will replace assets\Sessions.ico and assets\Sessions.png.
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\assets' }
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null

function New-RoundedRectangle([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap([int]$size) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 256.0
    # Back to front: lighter cards behind, the brand teal #0F766E in front.
    $cards = @(
        @{ X = 70; Y = 26; Color = [System.Drawing.Color]::FromArgb(255, 153, 200, 196) },
        @{ X = 48; Y = 62; Color = [System.Drawing.Color]::FromArgb(255, 63, 152, 145) },
        @{ X = 26; Y = 98; Color = [System.Drawing.Color]::FromArgb(255, 15, 118, 110) }
    )
    foreach ($card in $cards) {
        $path = New-RoundedRectangle ($card.X * $s) ($card.Y * $s) (160 * $s) (132 * $s) ([Math]::Max(2, 22 * $s))
        $brush = New-Object System.Drawing.SolidBrush $card.Color
        $g.FillPath($brush, $path)
        if ($size -ge 32) {
            $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(110, 255, 255, 255)), ([Math]::Max(1, 3 * $s))
            $g.DrawPath($pen, $path)
            $pen.Dispose()
        }
        $brush.Dispose(); $path.Dispose()
    }
    if ($size -ge 24) {
        # Two text lines on the front card.
        $line = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(235, 255, 255, 255))
        foreach ($row in @(@{ Y = 140; W = 104 }, @{ Y = 178; W = 68 })) {
            $p = New-RoundedRectangle (54 * $s) ($row.Y * $s) ($row.W * $s) (16 * $s) ([Math]::Max(1, 8 * $s))
            $g.FillPath($line, $p); $p.Dispose()
        }
        $line.Dispose()
    }
    $g.Dispose()
    return $bitmap
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$frames = foreach ($size in $sizes) {
    $bitmap = New-IconBitmap $size
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($size -eq 256) { $bitmap.Save((Join-Path $OutputDirectory 'Sessions.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bitmap.Dispose()
    [pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
}

$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $out
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($frame in $frames) {
    $dimension = if ($frame.Size -ge 256) { 0 } else { $frame.Size }
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
    $offset += $frame.Bytes.Length
}
foreach ($frame in $frames) { $writer.Write($frame.Bytes) }
$writer.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'Sessions.ico'), $out.ToArray())
Write-Output (Resolve-Path (Join-Path $OutputDirectory 'Sessions.ico')).Path
