# Convert a reference-core framebuffer dump (%TEMP%\ref_fb_<frame>.bin:
# two 256x192 32bpp screens) into a 24bpp BMP so it can be compared side by
# side with the local `--shot-every` BMPs.
#
# 21-B9yi(xu108m) IMPORTANT: melonDS's `GetFramebuffers(&top, &bottom)` hands
# back `Framebuffer[front][0]` first, and that one is the DS's **bottom** screen
# (measured: its stats match our bottom screen exactly).  So the bin layout is
# [screen A = DS bottom][screen B = DS top] and this script used to put the
# bottom screen on top, i.e. its output was vertically swapped relative to our
# `--shot` BMPs (top screen first).  Fixed here.
#
# NOTE: keep this file ASCII-only -- Windows PowerShell 5.1 reads .ps1 as ANSI
# and non-ASCII characters in comments can break parsing.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\fb2bmp.ps1 -In <bin> -Out <bmp>
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out
)

$w = 256
$h = 192
$total = $h * 2
$rowSize = (($w * 3) + 3) -band (-bnot 3)
$dataSize = $rowSize * $total
$fileSize = 54 + $dataSize

$b = [IO.File]::ReadAllBytes($In)
if ($b.Length -lt ($w * $total * 4)) {
    throw "input too small: $($b.Length) bytes"
}

$hdr = New-Object byte[] 54
$hdr[0] = 0x42; $hdr[1] = 0x4D
[BitConverter]::GetBytes([int]$fileSize).CopyTo($hdr, 2)
$hdr[10] = 54
$hdr[14] = 40
[BitConverter]::GetBytes([int]$w).CopyTo($hdr, 18)
[BitConverter]::GetBytes([int]$total).CopyTo($hdr, 22)
$hdr[26] = 1
$hdr[28] = 24
[BitConverter]::GetBytes([int]$dataSize).CopyTo($hdr, 34)

$rows = New-Object byte[] $dataSize
for ($y = 0; $y -lt $total; $y++) {
    $dispY = $total - 1 - $y
    for ($x = 0; $x -lt $w; $x++) {
        if ($dispY -lt $h) { $scr = 1; $yy = $dispY } else { $scr = 0; $yy = $dispY - $h }
        $off = ((($scr * $h) + $yy) * $w + $x) * 4
        $v = [BitConverter]::ToUInt32($b, $off)
        $s = ($y * $rowSize) + ($x * 3)
        $rows[$s + 0] = [byte](($v -shr 16) -band 0xFF)
        $rows[$s + 1] = [byte](($v -shr 8) -band 0xFF)
        $rows[$s + 2] = [byte]($v -band 0xFF)
    }
}

$fs = [IO.File]::Create($Out)
try {
    $fs.Write($hdr, 0, 54)
    $fs.Write($rows, 0, $dataSize)
} finally {
    $fs.Close()
}
Write-Output "wrote $Out"
