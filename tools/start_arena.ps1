# Test tool: the test arena - a research copy in an offline Multiverse Madness match (Santa's Workshop) with no enemies
# (FGUYMOD_TEST_ARENA=1, research.cpp), one player, full screen: a quiet place to look at things. Prints the copy's
# process id. -Right: how many characters to the right of the first one on the character select (0 Peter).
# -Enemies: the match's own enemies come as usual (something to shoot at).
#   start_arena.ps1 [-GameDir <research copy>] [-Right <n>] [-FirstPerson] [-Enemies]
param([string]$GameDir = "A:\Games\FGUY-Research", [int]$Right = 0, [switch]$FirstPerson, [switch]$Enemies,
      [string]$Res = "1920x1080")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
powershell -ExecutionPolicy Bypass -File tools\stop_tests.ps1 -GameDir $GameDir | Out-Null; Set-Location $root
Copy-Item "$root\build\dinput8.dll" "$GameDir\dinput8.dll" -Force
powershell -ExecutionPolicy Bypass -File tools\wait_gpu.ps1 -Seconds 240 | Out-Null
$env:FGUYMOD_TEST_ARENA = if ($Enemies) { "0" } else { "1" }
foreach ($try in 1..3) {   # (now and then the copy doesn't come up - no log after a while: started again)
    $h = & tools\start_test.ps1 -GameDir $GameDir -Res $Res
    $log = "$GameDir\FGUYMod\FGUYMod_$h.log"
    $until = (Get-Date).AddSeconds(20); while ((Get-Date) -lt $until -and -not (Test-Path $log)) { Start-Sleep 1 }
    if (Test-Path $log) { break }
    Stop-Process -Id $h -Force -ErrorAction SilentlyContinue; Start-Sleep 5
}
Remove-Item Env:FGUYMOD_TEST_ARENA
Start-Sleep 18
function Pad($b) { & tools\pad.ps1 -Id $h $b | Out-Null }
foreach ($b in "a", "start", "a") { Pad $b; Start-Sleep 4 }; Start-Sleep 2
foreach ($b in "down", "a", "down", "a", "a") { Pad $b; Start-Sleep 3 }   # (Multiplayer, Multiverse Madness, join)
foreach ($i in 1..$Right) { if ($Right -gt 0) { Pad right; Start-Sleep 1 } }
Pad a; Start-Sleep 3   # (the character)
Pad start
$until = (Get-Date).AddSeconds(90); while ((Get-Date) -lt $until -and -not (Select-String $log -Pattern 'hud.gfx' -SimpleMatch -Quiet)) { Start-Sleep 1 }
Start-Sleep 6; Pad a; Start-Sleep 8
if ($FirstPerson) {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$h", [System.IO.Pipes.PipeDirection]::Out)
    $p.Connect(3000); $w = New-Object System.IO.StreamWriter($p); $w.WriteLine("fpv 0 1"); $w.Flush(); $w.Dispose()
}
"$h"
