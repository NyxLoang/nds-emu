# 21-B9yi(xu113): scripted demonstration for the "battle LEADER panel long-press"
# acceptance item.  It does NOT decide the acceptance (that is the human looking
# at the game) -- it *proves the touch is delivered to the game* by diffing the
# per-frame screen fingerprints of two otherwise identical runs:
#
#   baseline : load build\battle.state, run N frames
#   press    : same, plus a touch held on the LEADER panel for >= 24 frames
#
# Any divergence in the fingerprint sequence means the game reacted to the touch.
# (The visible effect is a view/selection change; it can be transient, so look at
#  the fingerprints, not only the final screenshot.)
#
# NOTE: frame numbers of --touch-*/--key-* scripts are ABSOLUTE.  The battle state
# was saved at frame 12200, so the press must start at 12200+20 and not at 20.
#
# ASCII only (PowerShell 5.1 reads .ps1 as ANSI without a BOM).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\battle-touch-demo.ps1
#   powershell ... -File tools\battle-touch-demo.ps1 -Exe build\nds-emu.exe -TouchX 21 -TouchY 190 -Hold 40

param(
    [string]$Rom = "tools\rom_ascii.nds",
    [string]$State = "build\battle.state",
    [string]$Exe = "build-pgo\nds-emu.exe",
    [int]$Frames = 300,
    [int]$StartFrame = 12220,
    [int]$TouchX = 21,
    [int]$TouchY = 190,
    [int]$Hold = 40,
    [string]$Work = "build\touchdemo"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
if (-not (Test-Path $Exe))   { throw "exe not found: $Exe" }
if (-not (Test-Path $State)) { throw "state not found: $State (see docs/21-rom-bringup.md sec 108r for how to make it)" }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

$common = @($Rom, "--load-state", $State, "--headless-frames", "$Frames", "--screen-hash-every", "15")

Write-Host "[1/2] baseline run (no touch)"
$base = & $Exe @common 2>&1 | Select-String -Pattern 'screenhash' | ForEach-Object { $_.Line }

Write-Host "[2/2] long-press run (LEADER panel at $TouchX,$TouchY for $Hold frames from f=$StartFrame)"
$press = & $Exe @common --touch-frame $StartFrame --touch-x $TouchX --touch-y $TouchY --touch-period $Hold 2>&1 |
         Select-String -Pattern 'screenhash' | ForEach-Object { $_.Line }

$base | Set-Content -Encoding ascii (Join-Path $Work "hashes_base.txt")
$press | Set-Content -Encoding ascii (Join-Path $Work "hashes_press.txt")

$diff = Compare-Object $base $press
if (($diff | Measure-Object).Count -eq 0) {
    Write-Host "RESULT: identical fingerprints -- the touch produced NO visible change in this window."
    Write-Host "        (try a different moment, a longer hold, or a drag: --touch-drag X1,Y1,X2,Y2)"
} else {
    $first = ($diff | Select-Object -First 1).InputObject
    Write-Host "RESULT: fingerprints diverge => the game reacted to the touch."
    Write-Host "        first differing line: $first"
    Write-Host "        (details: $Work\hashes_base.txt vs $Work\hashes_press.txt)"
}
