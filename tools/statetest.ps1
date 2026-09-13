# 21-B9yi(续107): byte-exact save-state round-trip check ("save -> load -> run"
# must reproduce the continuous run byte for byte).
#
# Why a script: doing this by hand in a shell loop is error prone -- stale dump
# files from a previous iteration are easily compared by mistake, and a missing
# "dump" makes the check silently compare yesterday's data.  So: same directory
# for every artifact, freshness check on every file, and one PASS/FAIL line.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\statetest.ps1 `
#       -Rom tools\rom_ascii.nds -Frame 400 -Run 300 `
#       -Extra "--key-random,12345;--key-period,40"
#
# Semantics: the continuous run goes to frame (Frame + Run); the loaded run
# starts from a state saved at frame Frame and then runs Run more frames, so
# both end on the same frame and must have identical memory images.

param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [int]$Frame = 400,
    [int]$Run = 300,
    [string]$Exe = "build\nds-emu.exe",
    [string]$Work = "build\statetest",
    [string]$Extra = "",
    [switch]$KeepState,
    [switch]$DebugCmd
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Work)) { New-Item -ItemType Directory -Path $Work | Out-Null }
$state = Join-Path $Work ("at{0}.state" -f $Frame)
$prefixC = Join-Path $Work "cont"
$prefixL = Join-Path $Work "load"
$parts = @("mainram", "vram", "dtcm", "itcm", "arm7wram", "sharedwram", "palette", "io9", "io7")
# Extra emulator arguments, given as one semicolon separated string (``powershell
# -File`` cannot bind real arrays -- it would hand us a single quoted blob).
# Semicolon (not comma) because some options take comma separated values of their
# own, e.g. --touch-drag 64,128,150,90.
$extraArgs = @()
if ($Extra.Trim() -ne "") {
    $extraArgs = $Extra.Split(";") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
}

# Remove stale artifacts: if a run fails to write, we want a loud "missing file"
# instead of a silent comparison against the previous iteration.
Get-ChildItem $Work -Filter "*.bin" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem $Work -Filter "*.state" -ErrorAction SilentlyContinue | Remove-Item -Force

$t0 = Get-Date

Write-Host ("[1/3] continuous: run {0} frames (target frame {1})" -f ($Frame + $Run), ($Frame + $Run))
$argvC = @($Rom, "--headless-frames", "$($Frame + $Run)") + $extraArgs + @("--dump", $prefixC)
$outC = & $Exe @argvC 2>&1

Write-Host ("[2/3] save state at frame {0}" -f $Frame)
$argvS = @($Rom, "--headless-frames", "$Frame",
           "--state-save-frame", "$Frame", "--save-state", $state) + $extraArgs
$outS = & $Exe @argvS 2>&1

Write-Host ("[3/3] load state + run {0} frames" -f $Run)
$argvL = @($Rom, "--load-state", $state, "--headless-frames", "$Run") + $extraArgs + @("--dump", $prefixL)
if ($DebugCmd) { Write-Host ("  cmd: {0} {1}" -f $Exe, ($argvL -join " ")) }
$outL = & $Exe @argvL 2>&1

$fail = 0
foreach ($p in $parts) {
    $fc = "${prefixC}_$p.bin"
    $fl = "${prefixL}_$p.bin"
    foreach ($f in @($fc, $fl)) {
        if (-not (Test-Path $f)) {
            Write-Host ("  MISSING {0}" -f $f)
            if ($f -eq $fl) {
                Write-Host "  --- last lines of the loaded run ---"
                $outL | Select-Object -Last 12 | ForEach-Object { Write-Host ("  | {0}" -f $_) }
                Write-Host "  -----------------------------------"
            }
            $fail++
        }
    }
    if (-not (Test-Path $fc) -or -not (Test-Path $fl)) { continue }
    $a = [System.IO.File]::ReadAllBytes($fc)
    $b = [System.IO.File]::ReadAllBytes($fl)
    if ($a.Length -ne $b.Length) {
        Write-Host ("  {0,-10} SIZE MISMATCH {1} vs {2}" -f $p, $a.Length, $b.Length)
        $fail++
        continue
    }
    $n = $a.Length
    $diff = 0
    $first = -1
    $i = 0
    while ($i -lt $n) {
        if ($a[$i] -ne $b[$i]) {
            $s = $i
            while ($i -lt $n -and $a[$i] -ne $b[$i]) { $i++ }
            $diff += ($i - $s)
            if ($first -lt 0) { $first = $s }
        } else { $i++ }
    }
    $tag = if ($diff -eq 0) { "OK  " } else { "DIFF" }
    Write-Host ("  {0} {1,-10} diff={2,-8} first=0x{3:X}" -f $tag, $p, $diff, ([Math]::Max($first, 0)))
    if ($diff -ne 0) { $fail++ }
}

if (-not $KeepState) { Remove-Item $state -Force -ErrorAction SilentlyContinue }

if ($fail -eq 0) {
    Write-Host ("PASS: save/load at frame {0} + {1} frames is byte-exact" -f $Frame, $Run) -ForegroundColor Green
    exit 0
} else {
    Write-Host ("FAIL: {0} comparison(s) differ" -f $fail) -ForegroundColor Red
    exit 1
}
