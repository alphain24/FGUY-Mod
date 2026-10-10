# Online test on one PC: starts copies of the game (each with its own copy of a save and its own player
# name), walks each one to Main Menu > ONLINE, and prints their process ids. Needs a lobby server:
#   python server\fguy_lobby.py            (local)   or set -Server to your VPS address.
#   test_online.ps1 [-Players 2] [-Server 127.0.0.1:7650] [-Save <file.sav>]
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [int]$Players = 2,
    [string]$Server = "127.0.0.1:7650",
    [string]$Save = (Join-Path $PSScriptRoot "..\backups\Save\player0_default.sav")
)
& (Join-Path $PSScriptRoot "stop_tests.ps1") -GameDir $GameDir
$start = Join-Path $PSScriptRoot "start_test.ps1"
$pad = Join-Path $PSScriptRoot "pad.ps1"
$names = "Host", "Guest", "Third", "Fourth"
$ids = @()
foreach ($i in 0..($Players - 1)) {
    $dir = Join-Path $GameDir "FGUYMod\testsave$i"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $Save $dir -Force
    $env:FGUYMOD_SAVEDIR = "testsave$i"; $env:FGUYMOD_ONLINE_NAME = $names[$i]; $env:FGUYMOD_ONLINE_SERVER = $Server
    $ids += & $start -Res "960x540"
    Start-Sleep -Seconds 1
}
foreach ($v in "FGUYMOD_SAVEDIR", "FGUYMOD_ONLINE_NAME", "FGUYMOD_ONLINE_SERVER") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 42   # boot logos (pressing during the first ones crashes the game)
foreach ($id in $ids) {   # trailer -> title -> save slot -> Main Menu -> ONLINE
    & $pad -Id $id a | Out-Null; Start-Sleep 3
    & $pad -Id $id start | Out-Null; Start-Sleep 4
    & $pad -Id $id a | Out-Null; Start-Sleep 5
    & $pad -Id $id down down a | Out-Null; Start-Sleep 2
}
$ids
