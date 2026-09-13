# 21-B9yi(续91)：**主内存逐字节对账**（本地 vs 参考核）。
#
# 用途：回答「本地和参考核到底是不是同一个游戏状态」——这是所有跨核对照
#       （逐像素画面、逐帧音频）能不能下强结论的前提。
#
# 前提：参考核 harness 会在 f=20/30/50/80/120/180/250/350/500/590 把
#       %TEMP%\ref_f<帧>_mainram.bin 写出来（见 ref_harness.cpp）。
#       本脚本用本地模拟器跑到同一个帧号后 `--dump` 出主内存，再逐字节比较。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\ramcmp.ps1 `
#       -Rom "tools\Z 最终幻想12…nds" -Frame 80

param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [int]$Frame = 80,
    [string]$Exe = "build\nds-emu.exe",
    [string]$Work = "build",
    [int]$ShowRanges = 10
)

$ErrorActionPreference = "Stop"

$refPath = Join-Path $env:TEMP ("ref_f{0}_mainram.bin" -f $Frame)
if (-not (Test-Path $refPath)) {
    Write-Error "找不到参考核的内存镜像 $refPath（先跑一次 refhead，它会在固定帧号写出内存）"
}

$prefix = Join-Path $Work ("ramcmp_f{0}" -f $Frame)
& $Exe $Rom --headless-frames $Frame --dump $prefix | Out-Null
$locPath = "${prefix}_mainram.bin"
if (-not (Test-Path $locPath)) { Write-Error "本地 dump 未生成：$locPath" }

$a = [System.IO.File]::ReadAllBytes($refPath)
$b = [System.IO.File]::ReadAllBytes($locPath)
$n = [Math]::Min($a.Length, $b.Length)

$diff = 0
$first = -1
$ranges = New-Object System.Collections.ArrayList
$i = 0
while ($i -lt $n) {
    if ($a[$i] -ne $b[$i]) {
        $start = $i
        while ($i -lt $n -and $a[$i] -ne $b[$i]) { $i++ }
        $diff += ($i - $start)
        if ($first -lt 0) { $first = $start }
        if ($ranges.Count -lt $ShowRanges) {
            $rx = ($a[$start..([Math]::Min($i - 1, $start + 15))] | ForEach-Object { $_.ToString("X2") }) -join "-"
            $ry = ($b[$start..([Math]::Min($i - 1, $start + 15))] | ForEach-Object { $_.ToString("X2") }) -join "-"
            [void]$ranges.Add(("0x{0:X6} len={1}`n    ref={2}`n    loc={3}" -f $start, ($i - $start), $rx, $ry))
        }
    } else { $i++ }
}

Write-Host ("frame={0}  ref={1}  loc={2}" -f $Frame, $a.Length, $b.Length)
Write-Host ("差异字节 = {0} / {1}  ({2:F4}%)   首个差异偏移 = 0x{3:X}" -f `
    $diff, $n, (100.0 * $diff / $n), ([Math]::Max($first, 0)))
if ($ranges.Count -gt 0) {
    Write-Host "前若干差异区间（ref = 参考核，loc = 本地）："
    $ranges | ForEach-Object { Write-Host $_ }
}
