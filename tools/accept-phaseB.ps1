# 21-B9yi(xu114): one-command runner for the AUTOMATIC part of the Phase B
# milestone acceptance.  It runs every machine-checkable gate and prints one
# PASS/FAIL table with the authoritative numbers.  The two human items
# (in-game save, battle LEADER long-press by hand) are printed at the end.
#
# ASCII only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\accept-phaseB.ps1
#   powershell ... -File tools\accept-phaseB.ps1 -Rom tools\rom_ascii.nds -Play build-pgo\nds-emu.exe

param(
    [string]$Rom = "tools\rom_ascii.nds",
    [string]$Test = "build\test_nds.exe",
    [string]$Exe = "build\nds-emu.exe",
    [string]$Play = "build-pgo\nds-emu.exe",
    [string]$Work = "build\accept"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$anchorShotHash = "74D73A3D6552E9931559F062E26707B77F9CFFD6D499120C3DF5394AA6B58314"
$anchorSaveHash = "3A361368EF684AD7"
$results = New-Object System.Collections.Generic.List[string]

function Add-Result([string]$name, [bool]$ok, [string]$detail) {
    $tag = "FAIL"
    if ($ok) { $tag = "PASS" }
    $results.Add(("{0}  {1,-34} {2}" -f $tag, $name, $detail))
}

# --- 1) unit tests -----------------------------------------------------------
$testOut = & $Test 2>&1 | Out-String
$lines = $testOut -split "`r?`n" | Where-Object { $_ -match '\S' }
$nums = @()
if ($lines.Count -gt 0) {
    $nums = [regex]::Matches($lines[-1], "\d+") | ForEach-Object { $_.Value }
}
if ($nums.Count -ge 2) {
    $checks = $nums[$nums.Count - 2]
    $failsN = $nums[$nums.Count - 1]
    Add-Result "unit tests" ($failsN -eq "0") ("{0} checks, {1} failures" -f $checks, $failsN)
} else {
    Add-Result "unit tests" $false "could not parse output of $Test"
}

# --- 2) anchor (2000 frames) -------------------------------------------------
$shot = Join-Path $Work "anchor.bmp"
$anchorOut = & $Exe $Rom --headless-frames 2000 --stats-every 2000 --shot $shot `
    --key-frame 1 --key-mask 0x3FF --key-period 120 2>&1 | Out-String
$statsOk = $anchorOut -match "top nz=49152/49152 rgb=24,36,40 bot nz=49152/49152 rgb=52,81,108"
$saveOk = $anchorOut -match ("savechip:.*hash=" + $anchorSaveHash)
$hashStr = "no file"
if (Test-Path $shot) { $hashStr = (Get-FileHash $shot -Algorithm SHA256).Hash }
$hashOk = ($hashStr -eq $anchorShotHash)
Add-Result "anchor stats (f=2000)" $statsOk "expect top 24,36,40 / bot 52,81,108"
Add-Result "anchor savechip" $saveOk ("expect " + $anchorSaveHash)
Add-Result "anchor screenshot sha256" $hashOk $hashStr.Substring(0, 16)

# --- 3) state round-trip (byte exact) ---------------------------------------
$stateOut = & powershell -NoProfile -ExecutionPolicy Bypass -File tools\statetest.ps1 `
    -Rom $Rom -Frame 300 -Run 500 -Exe $Exe `
    -Extra "--key-frame,1;--key-mask,0x3FF;--key-period,120" 2>&1 | Out-String
$stateOk = $stateOut -match "PASS"
Add-Result "save-state round trip" $stateOk ($(if ($stateOk) { "byte-exact" } else { "see output" }))

# --- 4) touch reaches the game (battle LEADER long-press) -------------------
$demoOut = & powershell -NoProfile -ExecutionPolicy Bypass -File tools\battle-touch-demo.ps1 `
    -Rom $Rom -Exe $Play 2>&1 | Out-String
$demoOk = $demoOut -match "fingerprints diverge"
$first = [regex]::Match($demoOut, "first differing line: (.*)")
Add-Result "touch reaches battle UI" $demoOk ($(if ($demoOk) { $first.Groups[1].Value.Trim() } else { "no divergence" }))

# --- summary ----------------------------------------------------------------
Write-Host ""
Write-Host "=============== Phase B automatic acceptance ==============="
foreach ($r in $results) { Write-Host $r }
$fails = ($results | Where-Object { $_ -like "FAIL*" } | Measure-Object).Count
Write-Host "============================================================"
if ($fails -eq 0) {
    Write-Host "ALL AUTOMATIC CHECKS PASSED ($($results.Count) gates)"
} else {
    Write-Host "$fails AUTOMATIC CHECK(S) FAILED"
}
Write-Host ""
Write-Host "Manual items (by hand, use $Play):"
Write-Host "  A. in-game save : play to a save point, save, quit; the exit summary must show"
Write-Host "                    savechip nonzero(vs 0xFF) growing from 24, and a changed .sav"
Write-Host "  B. battle order : $Play $Rom --load-state build\battle.state --shot build\acc.bmp"
Write-Host "                    hold the mouse on the bottom-screen LEADER panel (21,190) >= 0.4s"
Write-Host "                    => top-screen 3D view switches and you can then give orders"
exit $fails
