# gen_font_atlas.ps1 - Generate a bitmap font atlas (TGA) for the on-screen stats overlay.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 files using the
# system ANSI code page when there is no BOM, so non-ASCII (e.g. Chinese) comments
# would be mis-decoded and break the parser.
#
# Why this script exists:
#   ImGui cannot be used in this environment (no network access to fetch it), so the
#   renderer draws its statistics with a tiny built-in text renderer. That renderer needs
#   a font atlas, and the simplest portable way to produce one here is .NET System.Drawing
#   with a monospaced system font.
#
# Output layout (must match DebugText.h):
#   16 columns x 6 rows of 8x16 cells = 128x96 pixels, 32-bit BGRA, top-left origin.
#   Cell index i (0..95) holds ASCII character (32 + i), i.e. ' ' .. '~'.

Add-Type -AssemblyName System.Drawing

$cellW = 8
$cellH = 16
$cols  = 16
$rows  = 6
$width  = $cols * $cellW   # 128
$height = $rows * $cellH   # 96

$outDir = Join-Path $PSScriptRoot "..\assets"
$outDir = [System.IO.Path]::GetFullPath($outDir)
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$outPath = Join-Path $outDir "font_atlas.tga"

$bmp = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(0, 0, 0, 0))
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None

# Consolas is monospaced and present on every Windows install.
$font = New-Object System.Drawing.Font("Consolas", 11.0, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$brush = [System.Drawing.Brushes]::White
$fmt = [System.Drawing.StringFormat]::GenericTypographic
$fmt.FormatFlags = $fmt.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

for ($i = 0; $i -lt ($cols * $rows); $i++) {
    $cx = [single](($i % $cols) * $cellW)
    $cy = [single]([math]::Floor($i / $cols) * $cellH)

    # The last cell maps to ASCII 127 (DEL), which is not printable, so it is
    # repurposed as a SOLID COLOR BLOCK: the debug text stretches its UV to draw
    # the translucent backing panel behind the statistics.
    if ($i -eq ($cols * $rows - 1)) {
        $g.FillRectangle([System.Drawing.Brushes]::White, $cx, $cy, [single]$cellW, [single]$cellH)
        continue
    }

    $ch = [char]($i + 32)
    $g.DrawString([string]$ch, $font, $brush, $cx, [single]($cy + 1), $fmt)
}
$g.Dispose()
$font.Dispose()

# Pull the pixels out as BGRA (Format32bppArgb is BGRA in memory on little-endian).
$rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                      [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$stride = $data.Stride
$pixels = New-Object byte[] ($stride * $height)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
$bmp.UnlockBits($data)
$bmp.Dispose()

# TGA header: 18 bytes, imageType 2 (uncompressed true-color), 32bpp, top-left origin.
$header = New-Object byte[] 18
$header[2]  = 2
$header[12] = [byte]($width  -band 0xFF)
$header[13] = [byte](($width  -shr 8) -band 0xFF)
$header[14] = [byte]($height -band 0xFF)
$header[15] = [byte](($height -shr 8) -band 0xFF)
$header[16] = 32
$header[17] = 0x28   # bit5 = top-left origin, low nibble = 8 alpha bits

$out = New-Object byte[] (18 + $width * $height * 4)
[System.Array]::Copy($header, 0, $out, 0, 18)
# Rows in the bitmap may be padded by stride; copy row by row so the TGA stays tightly packed.
for ($y = 0; $y -lt $height; $y++) {
    [System.Array]::Copy($pixels, $y * $stride, $out, 18 + $y * $width * 4, $width * 4)
}
[System.IO.File]::WriteAllBytes($outPath, $out)

Write-Output ("wrote {0} ({1}x{2}, {3} cells of {4}x{5})" -f $outPath, $width, $height, ($cols * $rows), $cellW, $cellH)
