# 21-B9yi(xu108): describe how two memory images differ (which regions, what stride).
#
# ASCII only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI unless the file
# has a UTF-8 BOM, so non-ASCII text here can break the parser (same reason
# tools/statetest.ps1 is English).
#
# Used when comparing our core against the melonDS reference dump, or two runs of
# our own.  Prints: total differing bytes, first offset, number of runs of
# differing bytes, the first N runs with sample values, a 64 KB page histogram and
# the stride distribution of the run starts.  A regular stride usually means "an
# array of structs where one field differs" (e.g. a per-entity state value).
#
# NOTE: the byte arrays live in $da/$db, *not* $a/$b -- PowerShell parameters are
# type constrained ($A/$B are [string] paths) and variable names are case
# insensitive, so assigning a byte[] to $a would silently stringify it.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ref\ramdiff.ps1 `
#       -A "$env:TEMP\ref_f285_mainram.bin" -B build\bis285_mainram.bin -ShowRanges 12

param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [int]$ShowRanges = 12,
    [int]$TopPages = 8,
    [int]$SampleBytes = 16
)

$ErrorActionPreference = "Stop"
$da = [System.IO.File]::ReadAllBytes($A)
$db = [System.IO.File]::ReadAllBytes($B)
if ($da.Length -ne $db.Length) {
    Write-Host ("size differs: A={0} B={1}" -f $da.Length, $db.Length)
}
$n = [Math]::Min($da.Length, $db.Length)

$total = 0
$first = -1
$starts = New-Object System.Collections.ArrayList
$lens = New-Object System.Collections.ArrayList
$pages = @{}
$i = 0
while ($i -lt $n) {
    if ($da[$i] -ne $db[$i]) {
        $s = $i
        while ($i -lt $n -and $da[$i] -ne $db[$i]) { $i++ }
        $len = $i - $s
        $total += $len
        if ($first -lt 0) { $first = $s }
        [void]$starts.Add($s)
        [void]$lens.Add($len)
        $p = [int][Math]::Floor($s / 65536)
        if (-not $pages.ContainsKey($p)) { $pages[$p] = 0 }
        $pages[$p] += $len
    } else { $i++ }
}

Write-Host ("A = {0}" -f $A)
Write-Host ("B = {0}" -f $B)
Write-Host ("diff bytes = {0} / {1}  ({2:F4}%)   first = 0x{3:X}   runs = {4}" -f `
    $total, $n, (100.0 * $total / $n), ([Math]::Max($first, 0)), $starts.Count)

if ($starts.Count -eq 0) { exit 0 }

Write-Host "first runs (R = A / L = B):"
for ($k = 0; $k -lt [Math]::Min($ShowRanges, $starts.Count); $k++) {
    $s = $starts[$k]
    $len = $lens[$k]
    $end = [Math]::Min($s + $SampleBytes - 1, $n - 1)
    $rs = ($da[$s..$end] | ForEach-Object { $_.ToString("X2") }) -join " "
    $ls = ($db[$s..$end] | ForEach-Object { $_.ToString("X2") }) -join " "
    Write-Host ("  0x{0:X6} len={1}" -f $s, $len)
    Write-Host ("      R: {0}" -f $rs)
    Write-Host ("      L: {0}" -f $ls)
}

Write-Host "64KB pages with most differing bytes:"
$pageList = $pages.GetEnumerator() | Sort-Object { $_.Value } -Descending | Select-Object -First $TopPages
foreach ($pg in $pageList) {
    Write-Host ("  0x{0:X6}  diff={1}" -f ($pg.Key * 65536), $pg.Value)
}

if ($starts.Count -ge 3) {
    $hist = @{}
    for ($k = 1; $k -lt $starts.Count; $k++) {
        $d = $starts[$k] - $starts[$k - 1]
        if ($hist.ContainsKey($d)) { $hist[$d]++ } else { $hist[$d] = 1 }
    }
    Write-Host "most common strides between run starts:"
    $strideList = $hist.GetEnumerator() | Sort-Object { $_.Value } -Descending | Select-Object -First 8
    foreach ($h in $strideList) {
        Write-Host ("  stride={0}  (x{1})" -f $h.Key, $h.Value)
    }
}
