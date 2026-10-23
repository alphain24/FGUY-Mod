# Solo offline multiplayer on one test copy: Multiverse Madness with one player (the game allows that mode
# solo), screenshot once the level runs - one full-screen view, no split, no black half.
#   test_solo_mp.ps1 [-Res 1920x1080] [-God]   (-God: god mode as soon as the level runs - nothing knocks the player down)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [string]$Res = "960x540", [switch]$God)
$P = Split-Path $PSScriptRoot -Parent
$pad = Join-Path $PSScriptRoot "pad.ps1"
& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir | Out-Null
& "$PSScriptRoot\wait_gpu.ps1"
$dir = Join-Path $GameDir "FGUYMod\testsave0"
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item (Join-Path $P "backups\Save\player0_default.sav") $dir -Force
$env:FGUYMOD_SAVEDIR = "testsave0"; $env:FGUYMOD_TEST_AUTOMENU = "main"
$id = & "$PSScriptRoot\start_test.ps1" -GameDir $GameDir -Res $Res
Remove-Item Env:FGUYMOD_SAVEDIR, Env:FGUYMOD_TEST_AUTOMENU
$log = Join-Path $GameDir "FGUYMod\FGUYMod_$id.log"
$until = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $until -and -not ((Test-Path $log) -and (Select-String $log -Pattern "at the main menu" -Quiet))) { Start-Sleep 1 }
Start-Sleep 2
foreach ($step in @(@("down", "a"), @("down", "a"), @("a"), @("a"), @("a"), @("start"))) {
    & $pad -Id $id @step | Out-Null
    Start-Sleep 3
}
$until = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $until -and -not (Select-String $log -Pattern "hud.gfx" -SimpleMatch -Quiet)) { Start-Sleep 1 }
if ($God) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$id", [System.IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(3000); $w = New-Object System.IO.StreamWriter($pipe); $w.WriteLine("god 1"); $w.Flush(); $w.Dispose()
}
Start-Sleep 10
& "$PSScriptRoot\window_shot.ps1" -Id $id -Out (Join-Path $P "build\shots\solo_mp.png")
"pid $id; in the level: $([bool](Select-String $log -Pattern 'hud.gfx' -SimpleMatch -Quiet))"
