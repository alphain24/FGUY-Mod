# Online match test on one PC: copies go to ONLINE (test_online.ps1), the first hosts a lobby, the others
# join and ready up, the host starts. Then it waits for every session game to show the match (the handoff)
# and prints what matters from their logs: handoff, loading music, sync checks.
#   (-Players 3 / 4: more players; the output's "lobby copies: host <id>, guest <id>" names the first two)
#   test_online_match.ps1 [-Story] [-Seconds 40]
# Needs a lobby server (python server\fguy_lobby.py). Leaves the session games running (stop_tests.ps1).
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [switch]$Story,              # story co-op instead of Deathmatch
    [int]$Seconds = 40,          # how long to let the match run before reading the logs
    [string[]]$Controls = @(),   # per copy: keyboard / controller
    [string]$Save = "",           # the save every copy starts from (default: test_online.ps1's)
    [string]$Res = "960x540",     # the copies' render size
    [int]$Players = 2
)
$Controls = @($Controls | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # ("a,b" from -File comes as one)
$pad = Join-Path $PSScriptRoot "pad.ps1"
function Press([int]$id, [string]$button) { & $pad -Id $id $button | Out-Null; Start-Sleep -Milliseconds 600 }
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }

$extra = @{}
if ($Save) { $extra.Save = $Save }
$ids = @(& (Join-Path $PSScriptRoot "test_online.ps1") -GameDir $GameDir -Players $Players -Controls $Controls -Res $Res @extra)
$ids = @($ids[($ids.Count - $Players)..($ids.Count - 1)])
$hostId = $ids[0]; $guestId = $ids[1]
$guests = @($ids[1..($ids.Count - 1)])
Write-Output "lobby copies: host $hostId, guest $guestId"
if ($Players -gt 2) { Write-Output "all lobby copies: $($ids -join ', ')" }
Start-Sleep 2
Press $hostId down; Press $hostId down; Press $hostId a          # HOST A GAME > CREATE LOBBY
Start-Sleep 4
if ($Story) { Press $hostId down; Press $hostId left }           # GAME MODE: back one = STORY CO-OP
foreach ($g in $guests) {
    Press $g rb; Start-Sleep 2; Press $g a                       # FIND A GAME > the first game
    Start-Sleep 3
    Press $g x                                                   # ready
}
$until = (Get-Date).AddSeconds(20)   # everyone's ready (the server has it) before the host starts
while ((Get-Date) -lt $until -and @($guests | Where-Object { -not (Select-String -Path (Log $_) -Pattern "lobby action ready" -SimpleMatch -Quiet) }).Count) { Start-Sleep 1 }
Start-Sleep 2
Press $hostId x                                                  # start

# The lobby copies start the session games; wait until both show the match.
$pidFile = Join-Path $GameDir "FGUYMod\test_pids.txt"
$until = (Get-Date).AddSeconds(150)
$sessions = @()
while ((Get-Date) -lt $until) {
    Start-Sleep 3
    $sessions = @(Get-Content $pidFile | ForEach-Object { [int]$_ } | Where-Object { $_ -gt 0 -and $ids -notcontains $_ } | Select-Object -Unique)
    $shown = @($sessions | Where-Object { (Test-Path (Log $_)) -and (Select-String -Path (Log $_) -Pattern "window shown" -SimpleMatch -Quiet) })
    if ($sessions.Count -ge $Players -and $shown.Count -ge $Players) { break }
}
Write-Output "session games: $($sessions -join ', ')"
Start-Sleep $Seconds
foreach ($id in $ids + $sessions) {
    Write-Output "== $id"
    Select-String -Path (Log $id) -Pattern "Sounds: (online|loading)|Handoff: (the match|window)|DESYNC|SplitView: 2|Online: the session" |
        ForEach-Object { $_.Line.Substring(15) } | Select-Object -First 8
    $sync = @(Select-String -Path (Log $id) -Pattern "in sync with").Count
    if ($sync) { Write-Output "  sync checks logged as in sync: $sync" }
}
