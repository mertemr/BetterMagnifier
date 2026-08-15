# =============================================================================
# make-icons.ps1 — pack the source artwork into the application and tray icons
# =============================================================================
#
# The source of truth is res\icon-source.png, a single 1024x1024 RGBA image
# with the rounded-square shape and its soft edge already baked into the alpha
# channel — that is what makes it usable as a Windows icon straight off, with
# no further masking. Edit that file (in whatever image editor) and rerun this
# script; do not hand-edit the .ico files, they are generated.
#
#   res\BetterMagnifier.ico   application and window class icon
#   res\tray-on.ico           tray icon while at least one monitor is magnified
#   res\tray-off.ico          tray icon while idle — a desaturated copy of the
#                             same source, not a separate piece of art
#
# ICO is written by hand — classic BITMAPINFOHEADER entries, one per size,
# 32bpp BGRA with straight (not premultiplied) alpha, which is what LoadIcon
# expects. PNG-compressed entries would be smaller but are only honoured from
# Vista on for the 256 px size, and there is nothing here worth that asymmetry.
#
#   .\tools\make-icons.ps1
# =============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo      = Split-Path -Parent $PSScriptRoot
$outDir    = Join-Path $repo 'res'
$sourceImg = Join-Path $outDir 'icon-source.png'

if (-not (Test-Path $outDir))    { New-Item -ItemType Directory -Path $outDir | Out-Null }
if (-not (Test-Path $sourceImg)) { throw "Source image not found: $sourceImg" }

# Every entry Windows may ask for. 256 is what Explorer uses at large icon
# sizes; 16 is the tray and the title bar, and it is the one that most needs
# to stay legible at a glance.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

# -----------------------------------------------------------------------------
# Resize-Source — the source image, downscaled to one size, as bottom-up BGRA.
#
# High-quality bicubic with a transparent canvas underneath: drawing straight
# onto an ARGB bitmap without clearing it first can pick up whatever garbage
# GDI+ left in that memory, which shows up as a faint fringe around the edges
# at small sizes. CompositingMode SourceCopy is what keeps the alpha channel
# itself resampled instead of blended against an assumed opaque background.
# -----------------------------------------------------------------------------
function Resize-Source
{
    param([System.Drawing.Image]$Source, [int]$Size)

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try
    {
        $g.CompositingMode    = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.DrawImage($Source, 0, 0, $Size, $Size)
    }
    finally { $g.Dispose() }

    return $bmp
}

# -----------------------------------------------------------------------------
# Get-BgraBytes — bottom-up BGRA rows out of a Format32bppArgb bitmap, with an
# optional desaturation pass for the tray-off state.
#
# Grayscale by luminance (Rec. 601 weights) rather than a flat neutral tint:
# it is a transform of the actual artwork, so the monitor glyph and the accent
# circle keep their relative contrast instead of flattening to one grey.
# -----------------------------------------------------------------------------
function Get-BgraBytes
{
    param([System.Drawing.Bitmap]$Bitmap, [switch]$Desaturate)

    $w = $Bitmap.Width
    $h = $Bitmap.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $Bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                             [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)

    # GDI+'s Format32bppArgb is already top-down BGRA in memory — exactly the
    # byte layout an ICO's XOR mask wants, except ICO rows go bottom-up. So the
    # copy below both extracts the pixels and flips row order in one pass.
    $stride = $data.Stride
    $src = New-Object 'byte[]' ($stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $src, 0, $src.Length)
    $Bitmap.UnlockBits($data)

    $out = New-Object 'byte[]' ($w * $h * 4)

    for ($y = 0; $y -lt $h; $y++)
    {
        $srcRow = $y * $stride
        $dstRow = ($h - 1 - $y) * $w * 4   # bottom-up

        for ($x = 0; $x -lt $w; $x++)
        {
            $si = $srcRow + $x * 4
            $di = $dstRow + $x * 4

            $b = $src[$si + 0]; $g = $src[$si + 1]; $r = $src[$si + 2]; $a = $src[$si + 3]

            if ($Desaturate)
            {
                $lum = [byte][Math]::Round(0.114 * $b + 0.587 * $g + 0.299 * $r)
                $b = $lum; $g = $lum; $r = $lum
            }

            $out[$di + 0] = $b
            $out[$di + 1] = $g
            $out[$di + 2] = $r
            $out[$di + 3] = $a
        }
    }

    return $out
}

function Write-Ico
{
    param([string]$Path, [System.Drawing.Image]$Source, [switch]$Desaturate)

    $images = New-Object 'System.Collections.Generic.List[byte[]]'

    foreach ($s in $sizes)
    {
        $resized = Resize-Source -Source $Source -Size $s
        $xor = Get-BgraBytes -Bitmap $resized -Desaturate:$Desaturate
        $resized.Dispose()

        # The AND mask is required by the format even at 32bpp, where the alpha
        # channel is what actually decides transparency. All zeros means "every
        # pixel opaque, use the alpha"; rows are padded to four bytes.
        $maskStride = [int]([Math]::Floor(($s + 31) / 32)) * 4
        $mask = New-Object 'byte[]' ($maskStride * $s)

        $header = New-Object 'byte[]' 40
        [BitConverter]::GetBytes([int]40).CopyTo($header, 0)       # biSize
        [BitConverter]::GetBytes([int]$s).CopyTo($header, 4)       # biWidth
        [BitConverter]::GetBytes([int]($s * 2)).CopyTo($header, 8) # biHeight: XOR + AND
        [BitConverter]::GetBytes([int16]1).CopyTo($header, 12)     # biPlanes
        [BitConverter]::GetBytes([int16]32).CopyTo($header, 14)    # biBitCount
        # biCompression BI_RGB, and every size field left zero, which is legal
        # for BI_RGB and is what the shell writes itself.

        $img = New-Object 'byte[]' ($header.Length + $xor.Length + $mask.Length)
        [Array]::Copy($header, 0, $img, 0, $header.Length)
        [Array]::Copy($xor, 0, $img, $header.Length, $xor.Length)
        [Array]::Copy($mask, 0, $img, $header.Length + $xor.Length, $mask.Length)
        $images.Add($img)
    }

    $count = $images.Count
    $stream = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($stream)

    $w.Write([int16]0)          # reserved
    $w.Write([int16]1)          # type: icon
    $w.Write([int16]$count)

    # Directory entries come first, so every offset has to be known before any
    # image is written.
    $offset = 6 + 16 * $count
    for ($i = 0; $i -lt $count; $i++)
    {
        $s = $sizes[$i]
        # 256 is stored as 0: the field is one byte and 256 does not fit.
        $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
        $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
        $w.Write([byte]0)       # palette entries: none at 32bpp
        $w.Write([byte]0)       # reserved
        $w.Write([int16]1)      # planes
        $w.Write([int16]32)     # bit count
        $w.Write([int]$images[$i].Length)
        $w.Write([int]$offset)
        $offset += $images[$i].Length
    }

    foreach ($img in $images) { $w.Write($img) }

    $w.Flush()
    [System.IO.File]::WriteAllBytes($Path, $stream.ToArray())
    $w.Dispose()
    $stream.Dispose()

    Write-Host ("  {0}  ({1:N0} bytes, {2} sizes)" -f (Split-Path -Leaf $Path), (Get-Item $Path).Length, $count)
}

Write-Host '==> Writing icons'
$source = [System.Drawing.Image]::FromFile($sourceImg)
try
{
    Write-Ico -Path (Join-Path $outDir 'BetterMagnifier.ico') -Source $source
    Write-Ico -Path (Join-Path $outDir 'tray-on.ico')         -Source $source
    Write-Ico -Path (Join-Path $outDir 'tray-off.ico')        -Source $source -Desaturate
}
finally { $source.Dispose() }
Write-Host '==> Done'
