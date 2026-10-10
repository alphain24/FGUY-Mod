# One copy, local 2-player Deathmatch (pad 0 + virtual pad 1), for testing in-match features.
#   test_local_mp.ps1 [-FullView 1]   (FullView: show that viewport full screen, like online play)
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [int]$FullView = -1,
    [string]$Save = (Join-Path $PSScriptRoot "..\backups\Save\player0_default.sav")
)
& (Join-Path $PSScriptRoot "stop_tests.ps1") -GameDir $GameDir
$dir = Join-Path $GameDir "FGUYMod\testsave0"
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $Save $dir -Force
$env:FGUYMOD_SAVEDIR = "testsave0"
if ($FullView -ge 0) { $env:FGUYMOD_FULLVIEW = "$FullView" }
$id = & (Join-Path $PSScriptRoot "start_test.ps1") -Res "960x540"
Remove-Item Env:FGUYMOD_SAVEDIR, Env:FGUYMOD_FULLVIEW -ErrorAction SilentlyContinue
$pad = Join-Path $PSScriptRoot "pad.ps1"
function P([int]$p, [string[]]$b) { & $pad -Id $id -Pad $p @b | Out-Null }
Start-Sleep -Seconds 52                         # boot logos: never press during them (crash)
P 0 a; Start-Sleep 3; P 0 start; Start-Sleep 4; P 0 a; Start-Sleep 6       # trailer, title, save slot
P 0 down, a; Start-Sleep 4; P 0 a; Start-Sleep 4; P 0 a; Start-Sleep 5    # Multiplayer > Deathmatch > Next
P 1 a; Start-Sleep 2                            # player 2 joins
P 0 a; Start-Sleep 1; P 1 a; Start-Sleep 3                                 # teams
P 0 a; Start-Sleep 1; P 1 a; Start-Sleep 3                                 # characters
Start-Sleep 2; P 1 a; Start-Sleep 2                                   # player 2's character confirm (slow to load)
P 0 start                                       # start the match
$id
