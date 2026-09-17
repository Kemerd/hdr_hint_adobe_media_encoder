<#
.SYNOPSIS
    Generates resources/HdrHint.ico (16/24/32/48/64/256 px) and the four 23 px
    CEP panel icons. Pure System.Drawing, no external tools.
#>
param(
    [string]$IcoPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "resources\HdrHint.ico"),
    [string]$CepIconDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "cep\com.everett.hdrhint\icons")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

# Draws the app mark: a rounded gradient square with a bold white "H".
function New-MarkBitmap([int]$size, [bool]$dim) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded-rectangle path (radius ~22% of the size, like a macOS app tile).
    $r = [Math]::Max(2, [int]($size * 0.22))
    $d = $r * 2
    $w = $size - 1
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $w - $d, $d, $d, 0, 90)
    $path.AddArc(0, $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    # Purple -> blue diagonal gradient (dimmed variant for the CEP "normal" state).
    $c1 = if ($dim) { [System.Drawing.Color]::FromArgb(255, 140, 80, 200) } else { [System.Drawing.Color]::FromArgb(255, 176, 90, 255) }
    $c2 = if ($dim) { [System.Drawing.Color]::FromArgb(255, 20, 110, 210) } else { [System.Drawing.Color]::FromArgb(255, 10, 132, 255) }
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.Point]::new(0, 0), [System.Drawing.Point]::new($w, $w), $c1, $c2)
    $g.FillPath($brush, $path)

    # The letter.
    $fontSize = [Math]::Max(6, [int]($size * 0.58))
    $font = [System.Drawing.Font]::new("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = [System.Drawing.StringFormat]::new()
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $layout = [System.Drawing.RectangleF]::new(0, [float]($size * 0.03), $size, $size)
    $g.DrawString("H", $font, [System.Drawing.Brushes]::White, $layout, $sf)

    $g.Dispose(); $brush.Dispose(); $font.Dispose(); $path.Dispose()
    return $bmp
}

# --- ICO container (PNG-compressed entries are valid since Vista) ---------------
$sizes = 16, 24, 32, 48, 64, 256
$entries = @()
foreach ($s in $sizes) {
    $bmp = New-MarkBitmap $s $false
    $ms = [System.IO.MemoryStream]::new()
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += [pscustomobject]@{ Size = $s; Bytes = $ms.ToArray() }
    $bmp.Dispose(); $ms.Dispose()
}

$out = [System.IO.MemoryStream]::new()
$bw = [System.IO.BinaryWriter]::new($out)
$bw.Write([uint16]0)                 # reserved
$bw.Write([uint16]1)                 # type: icon
$bw.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim)   # width, height (0 = 256)
    $bw.Write([byte]0); $bw.Write([byte]0)         # palette, reserved
    $bw.Write([uint16]1); $bw.Write([uint16]32)    # planes, bpp
    $bw.Write([uint32]$e.Bytes.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $bw.Write($e.Bytes) }
$bw.Flush()
New-Item -ItemType Directory -Force (Split-Path -Parent $IcoPath) | Out-Null
[System.IO.File]::WriteAllBytes($IcoPath, $out.ToArray())
$bw.Dispose(); $out.Dispose()
Write-Host ("wrote " + $IcoPath + " (" + (Get-Item $IcoPath).Length + " bytes)")

# --- CEP panel icons (23 px) ----------------------------------------------------
New-Item -ItemType Directory -Force $CepIconDir | Out-Null
$variants = @(
    @{ Name = "hdrhint_23.png";               Dim = $true  },
    @{ Name = "hdrhint_23_rollover.png";      Dim = $false },
    @{ Name = "hdrhint_23_dark.png";          Dim = $true  },
    @{ Name = "hdrhint_23_dark_rollover.png"; Dim = $false }
)
foreach ($v in $variants) {
    $bmp = New-MarkBitmap 23 $v.Dim
    $p = Join-Path $CepIconDir $v.Name
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("wrote " + $p)
}
