# Crop a region of a 24bpp BMP and scale it up (nearest neighbour) into a new BMP.
# Used to inspect small details (e.g. the 8x8 dialog font) in emulator screenshots.
#
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads .ps1 as ANSI).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\bmpcrop.ps1 `
#          -In build\shot.bmp -Out build\zoom.bmp -X 32 -Y 200 -W 192 -H 120 -Scale 4
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$X = 0,
    [int]$Y = 0,
    [int]$W = 128,
    [int]$H = 96,
    [int]$Scale = 4
)

$b = [IO.File]::ReadAllBytes($In)
if ($b[0] -ne 0x42 -or $b[1] -ne 0x4D) { throw "not a BMP: $In" }
$dataOff = [BitConverter]::ToInt32($b, 10)
$srcW = [BitConverter]::ToInt32($b, 18)
$srcH = [BitConverter]::ToInt32($b, 22)
$bpp = [BitConverter]::ToInt16($b, 28)
if ($bpp -ne 24) { throw "expected 24bpp, got $bpp" }
$srcRow = (($srcW * 3) + 3) -band (-bnot 3)

if ($X -lt 0) { $X = 0 }
if ($Y -lt 0) { $Y = 0 }
if (($X + $W) -gt $srcW) { $W = $srcW - $X }
if (($Y + $H) -gt $srcH) { $H = $srcH - $Y }

$outW = $W * $Scale
$outH = $H * $Scale
$outRow = (($outW * 3) + 3) -band (-bnot 3)
$outData = $outRow * $outH
$outFile = 54 + $outData

$hdr = New-Object byte[] 54
$hdr[0] = 0x42; $hdr[1] = 0x4D
[BitConverter]::GetBytes([int]$outFile).CopyTo($hdr, 2)
$hdr[10] = 54
$hdr[14] = 40
[BitConverter]::GetBytes([int]$outW).CopyTo($hdr, 18)
[BitConverter]::GetBytes([int]$outH).CopyTo($hdr, 22)
$hdr[26] = 1
$hdr[28] = 24
[BitConverter]::GetBytes([int]$outData).CopyTo($hdr, 34)

$rows = New-Object byte[] $outData
for ($oy = 0; $oy -lt $outH; $oy++) {
    $srcYDown = $Y + [int][math]::Floor($oy / $Scale)       # image space, bottom-up source
    $srcYFile = $srcH - 1 - $srcYDown                        # file row (bottom-up)
    $dstRow = $outH - 1 - $oy
    for ($ox = 0; $ox -lt $outW; $ox++) {
        $srcX = $X + [int][math]::Floor($ox / $Scale)
        $so = $dataOff + ($srcYFile * $srcRow) + ($srcX * 3)
        $do = ($dstRow * $outRow) + ($ox * 3)
        $rows[$do + 0] = $b[$so + 0]
        $rows[$do + 1] = $b[$so + 1]
        $rows[$do + 2] = $b[$so + 2]
    }
}

$fs = [IO.File]::Create($Out)
try {
    $fs.Write($hdr, 0, 54)
    $fs.Write($rows, 0, $outData)
} finally {
    $fs.Close()
}
Write-Output "wrote $Out (${outW}x${outH})"
