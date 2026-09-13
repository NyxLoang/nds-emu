# 21-B9yi(xu108m): compare our `--shot` BMP against a reference framebuffer dump.
#
# The reference dump (%TEMP%\ref_fb_<frame>.bin) holds two 256x192 32bpp screens,
# and melonDS hands back the DS's **bottom** screen first (see fb2bmp.ps1); our
# `--shot` BMP stacks the **top** screen first.  This script knows that mapping,
# so it compares like with like and prints per-screen diff bytes and MAE.
#
# ASCII only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\fbcmp.ps1 `
#       -Ref "$env:TEMP\ref_fb_250.bin" -Bmp build\loc250.bmp

param(
    [Parameter(Mandatory = $true)][string]$Ref,
    [Parameter(Mandatory = $true)][string]$Bmp,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$w = 256; $h = 192
$rb = [IO.File]::ReadAllBytes($Ref)
if ($rb.Length -lt (2 * $w * $h * 4)) { throw "reference dump too small: $($rb.Length)" }
$bb = [IO.File]::ReadAllBytes($Bmp)
$bmpOff = [BitConverter]::ToInt32($bb, 10)
$bmpW = [BitConverter]::ToInt32($bb, 18)
$bmpH = [BitConverter]::ToInt32($bb, 22)
if ($bmpW -ne $w -or $bmpH -ne (2 * $h)) { throw "unexpected shot size: ${bmpW}x${bmpH}" }
$row = ($w * 3 + 3) -band (-bnot 3)

$fail = 0
foreach ($screen in 0, 1) {
    # ref screen 0 = DS bottom (our BMP rows h..2h-1); ref screen 1 = DS top (rows 0..h-1)
    $base = if ($screen -eq 0) { $h } else { 0 }
    $diff = 0; $mae = 0.0; $n = 0
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $w; $x++) {
            $o = ($screen * $h + $y) * $w * 4 + $x * 4
            $v = [BitConverter]::ToUInt32($rb, $o)
            $r = [int]($v -band 0xFF); $g = [int](($v -shr 8) -band 0xFF); $b = [int](($v -shr 16) -band 0xFF)
            $bo = $bmpOff + ((2 * $h) - 1 - ($base + $y)) * $row + $x * 3
            $br = [int]$bb[$bo + 2]; $bg = [int]$bb[$bo + 1]; $bbb = [int]$bb[$bo + 0]
            if ($r -ne $br -or $g -ne $bg -or $b -ne $bbb) { $diff++ }
            $mae += [Math]::Abs($r - $br) + [Math]::Abs($g - $bg) + [Math]::Abs($b - $bbb)
            $n += 3
        }
    }
    $name = if ($screen -eq 0) { "bottom" } else { "top   " }
    if ($diff -ne 0) { $fail++ }
    if (-not $Quiet) {
        $totalPx = $w * $h
        $maeAvg = $mae / $n
        Write-Host ("  {0} differ={1} of {2}   MAE={3:F4}" -f $name, $diff, $totalPx, $maeAvg)
    }
}

if ($fail -eq 0) {
    Write-Host "PASS: both screens are pixel-identical to the reference framebuffer" -ForegroundColor Green
    exit 0
} else {
    Write-Host "FAIL: $fail screen(s) differ" -ForegroundColor Red
    exit 1
}
