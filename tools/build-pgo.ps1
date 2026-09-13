# 21-B9yi(xu111): two-phase PGO build ("profile-guided optimization").
#
# Why: the window build was ~55.6 fps in the battle scene (emulation core 15.0 ms
# per frame vs the 16.72 ms real-hardware budget).  PGO on the interpreter gave
# -21.5% wall time on the same workload and **byte-identical** behaviour
# (anchor screenshot hash + savechip hash unchanged, state round-trip PASS),
# which puts the battle scene at 59.9 fps.
#
# The trick: BOTH phases must use the SAME build directory, because gcc looks
# for the .gcda files next to the objects it is compiling.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-pgo.ps1
#   powershell ... -File tools\build-pgo.ps1 -Rom tools\rom_ascii.nds -BattleFrames 2000 -BootFrames 2000
#
# Result: build-pgo\nds-emu.exe  (use it for smooth 60 fps play/acceptance)

param(
    [string]$Rom = "tools\rom_ascii.nds",
    [string]$State = "build\battle.state",
    [int]$BattleFrames = 2000,
    [int]$BootFrames = 2000,
    [string]$BuildDir = "build-pgo"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$cmake = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\cmake.exe"
if (-not (Test-Path $cmake)) { throw "cmake not found: $cmake" }

Write-Host "[1/3] configure + build with -fprofile-generate"
& $cmake -S . -B $BuildDir -DNDS_PROF=OFF -DCMAKE_C_FLAGS="-fprofile-generate" | Out-Null
& $cmake --build $BuildDir --target nds-emu | Out-Null
$exe = Join-Path $BuildDir "nds-emu.exe"
if (-not (Test-Path $exe)) { throw "build failed: $exe" }

Write-Host "[2/3] collect profile data (battle scene + boot)"
if (Test-Path $State) {
    & $exe $Rom --load-state $State --headless-frames $BattleFrames | Out-Null
    Write-Host "      battle workload done ($BattleFrames frames)"
} else {
    Write-Host "      no state file ($State) -- skipping battle workload"
}
if (Test-Path $Rom) {
    & $exe $Rom --headless-frames $BootFrames --key-random 12345 --key-period 40 `
        --touch-random 999 --touch-period 100 | Out-Null
    Write-Host "      boot workload done ($BootFrames frames)"
} else {
    throw "ROM not found: $Rom"
}

Write-Host "[3/3] rebuild with -fprofile-use"
& $cmake -S . -B $BuildDir -DNDS_PROF=OFF -DCMAKE_C_FLAGS="-fprofile-use -fprofile-correction" | Out-Null
& $cmake --build $BuildDir --target nds-emu | Out-Null
if (-not (Test-Path $exe)) { throw "PGO rebuild failed: $exe" }

Write-Host "done: $exe"
Write-Host "      (warnings about missing profile data are expected for third-party SDL files)"
