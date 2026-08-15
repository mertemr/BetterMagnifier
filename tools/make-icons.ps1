# =============================================================================
# make-icons.ps1 — generate the application and tray icons
# =============================================================================
#
# The icons are drawn here rather than checked in as binaries somebody has to
# open an editor to change. Three of them differ only by colour, and a generator
# keeps that fact visible: change the palette below and rerun.
#
#   res\BetterMagnifier.ico   application and window class icon
#   res\tray-on.ico           tray icon while at least one monitor is magnified
#   res\tray-off.ico          tray icon while idle
#
# ICO is written by hand — classic BITMAPINFOHEADER entries, one per size, 32bpp
# BGRA with straight (not premultiplied) alpha, which is what LoadIcon expects.
# PNG-compressed entries would be smaller but are only honoured from Vista on
# for the 256 px size, and there is nothing here worth that asymmetry.
#
#   .\tools\make-icons.ps1
# =============================================================================

$ErrorActionPreference = 'Stop'

$repo   = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $repo 'res'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

# Every entry Windows may ask for. 256 is the one Explorer uses at large icon
# sizes; 16 is the tray and the title bar, and it is the one that actually has
# to stay legible, which is why the glyph below is deliberately chunky.
$sizes = @(16, 24, 32, 48, 64, 128, 256)

# -----------------------------------------------------------------------------
# The glyph: a magnifying glass, in fractions of the icon's extent.
#
# Kept off centre toward the top left so the handle has room to run to the
# bottom right corner without the whole thing shrinking to make space.
# -----------------------------------------------------------------------------
$lensCx     = 0.415
$lensCy     = 0.395
$lensOuter  = 0.310    # outer edge of the ring
$lensRing   = 0.088    # ring thickness
$handleEnd  = 0.855    # both axes: the handle runs at 45 degrees
$handleHalf = 0.070    # half width

# Supersampling factor per axis. 4 is enough that a 16 px icon reads as smooth;
# beyond that the cost grows as the square and nothing looks different.
$ss = 4

function New-IconImage
{
    param(
        [int]$Size,
        [byte[]]$Stroke,     # B, G, R for the ring and the handle
        [byte[]]$Fill,       # B, G, R for the lens interior
        [double]$FillAlpha
    )

    # Bottom-up BGRA, which is what the DIB in an ICO is.
    $pixels = New-Object 'byte[]' ($Size * $Size * 4)

    $cx = $lensCx * $Size
    $cy = $lensCy * $Size
    $rOuter = $lensOuter * $Size
    $rInner = ($lensOuter - $lensRing) * $Size
    $hx = $handleEnd * $Size
    $hy = $handleEnd * $Size
    $hw = $handleHalf * $Size

    # The handle starts on the ring so the two read as one object, and is drawn
    # as a capsule: distance to the segment, thresholded. Rounded ends fall out
    # of that for free, which is what keeps it from looking cut off.
    $t0 = ($lensOuter - $lensRing * 0.5)
    $sx = $cx + $t0 * $Size * 0.7071
    $sy = $cy + $t0 * $Size * 0.7071

    $dxSeg = $hx - $sx
    $dySeg = $hy - $sy
    $segLenSq = $dxSeg * $dxSeg + $dySeg * $dySeg

    for ($y = 0; $y -lt $Size; $y++)
    {
        for ($x = 0; $x -lt $Size; $x++)
        {
            $covStroke = 0.0
            $covFill   = 0.0

            for ($sy2 = 0; $sy2 -lt $ss; $sy2++)
            {
                for ($sx2 = 0; $sx2 -lt $ss; $sx2++)
                {
                    $px = $x + ($sx2 + 0.5) / $ss
                    $py = $y + ($sy2 + 0.5) / $ss

                    $dx = $px - $cx
                    $dy = $py - $cy
                    $d  = [Math]::Sqrt($dx * $dx + $dy * $dy)

                    $inRing = ($d -le $rOuter -and $d -ge $rInner)
                    $inLens = ($d -lt $rInner)

                    # Distance from the sample to the handle segment.
                    $inHandle = $false
                    if ($segLenSq -gt 0)
                    {
                        $t = (($px - $sx) * $dxSeg + ($py - $sy) * $dySeg) / $segLenSq
                        if ($t -lt 0) { $t = 0 } elseif ($t -gt 1) { $t = 1 }
                        $qx = $sx + $t * $dxSeg
                        $qy = $sy + $t * $dySeg
                        $hd = [Math]::Sqrt(($px - $qx) * ($px - $qx) + ($py - $qy) * ($py - $qy))
                        $inHandle = ($hd -le $hw)
                    }

                    if ($inRing -or $inHandle) { $covStroke += 1.0 }
                    elseif ($inLens)           { $covFill   += 1.0 }
                }
            }

            $total = [double]($ss * $ss)
            $covStroke /= $total
            $covFill   /= $total

            if ($covStroke -le 0 -and $covFill -le 0) { continue }

            # Stroke over fill, both against a transparent ground. Compositing
            # in one step rather than blending two passes keeps the antialiased
            # boundary between them from picking up a dark seam.
            $aStroke = $covStroke
            $aFill   = $covFill * $FillAlpha
            $a = $aStroke + $aFill * (1.0 - $aStroke)

            if ($a -le 0.0005) { continue }

            $b = ($Stroke[0] * $aStroke + $Fill[0] * $aFill * (1.0 - $aStroke)) / $a
            $g = ($Stroke[1] * $aStroke + $Fill[1] * $aFill * (1.0 - $aStroke)) / $a
            $r = ($Stroke[2] * $aStroke + $Fill[2] * $aFill * (1.0 - $aStroke)) / $a

            # Bottom-up: row 0 of the DIB is the bottom row of the image.
            $row = $Size - 1 - $y
            $o = ($row * $Size + $x) * 4

            $pixels[$o + 0] = [byte][Math]::Round([Math]::Min(255.0, $b))
            $pixels[$o + 1] = [byte][Math]::Round([Math]::Min(255.0, $g))
            $pixels[$o + 2] = [byte][Math]::Round([Math]::Min(255.0, $r))
            $pixels[$o + 3] = [byte][Math]::Round([Math]::Min(255.0, $a * 255.0))
        }
    }

    return $pixels
}

function Write-Ico
{
    param(
        [string]$Path,
        [byte[]]$Stroke,
        [byte[]]$Fill,
        [double]$FillAlpha
    )

    # A typed list, and the copies below are deliberate. PowerShell's + on two
    # byte[] produces an Object[], which still has a Length and still indexes,
    # so the directory came out arithmetically consistent and the file was 125
    # bytes of pure header. Silent, and only visible as an icon that will not
    # load.
    $images = New-Object 'System.Collections.Generic.List[byte[]]'

    foreach ($s in $sizes)
    {
        $xor = New-IconImage -Size $s -Stroke $Stroke -Fill $Fill -FillAlpha $FillAlpha

        # The AND mask is required by the format even at 32bpp, where the alpha
        # channel is what actually decides transparency. All zeros means "every
        # pixel opaque, use the alpha"; rows are padded to four bytes.
        $maskStride = [int]([Math]::Floor(($s + 31) / 32)) * 4
        $mask = New-Object 'byte[]' ($maskStride * $s)

        $header = New-Object 'byte[]' 40
        [BitConverter]::GetBytes([int]40).CopyTo($header, 0)      # biSize
        [BitConverter]::GetBytes([int]$s).CopyTo($header, 4)      # biWidth
        [BitConverter]::GetBytes([int]($s * 2)).CopyTo($header, 8) # biHeight: XOR + AND
        [BitConverter]::GetBytes([int16]1).CopyTo($header, 12)    # biPlanes
        [BitConverter]::GetBytes([int16]32).CopyTo($header, 14)   # biBitCount
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

# BGR, because that is the order they go into the file in.
$accent = [byte[]](255, 194, 76)     # #4CC2FF — the "on" blue
$idle   = [byte[]](152, 143, 138)    # #8A8F98 — a neutral that survives both
                                     #           a light and a dark taskbar

Write-Host '==> Writing icons'
Write-Ico -Path (Join-Path $outDir 'BetterMagnifier.ico') -Stroke $accent -Fill $accent -FillAlpha 0.20
Write-Ico -Path (Join-Path $outDir 'tray-on.ico')         -Stroke $accent -Fill $accent -FillAlpha 0.20
Write-Ico -Path (Join-Path $outDir 'tray-off.ico')        -Stroke $idle   -Fill $idle   -FillAlpha 0.14
Write-Host '==> Done'
