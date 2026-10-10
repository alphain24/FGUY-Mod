# Online match test on one PC: two copies go to ONLINE (test_online.ps1), the first hosts a lobby, the second
# joins and readies up, the host starts. Then it waits for both session games to show the match (the
# handoff) and prints what matters from their logs: handoff, loading music, sync checks.
#   test_online_match.ps1 [-Story] [-Seconds 40]
# Needs a lobby server (python server\fguy_lobby.py). Leaves the session games running (stop_tests.ps1).
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [switch]$Story,              # story co-op instead of Deathmatch
    [int]$Seconds = 40,          # how long to let the match run before reading the logs
    [string[]]$Controls = @()    # per copy: keyboard / controller
)
$pad = Join-Path $PSScriptRoot "pad.ps1"
function Press([int]$id, [string]$button) { & $pad -Id $id $button | Out-Null; Start-Sleep -Milliseconds 600 }
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }

$ids = & (Join-Path $PSScriptRoot "test_online.ps1") -GameDir $GameDir -Players 2 -Controls $Controls
$hostId = $ids[-2]; $guestId = $ids[-1]
Write-Output "lobby copies: host $hostId, guest $guestId"
Start-Sleep 2
Press $hostId down; Press $hostId down; Press $hostId a          # HOST A GAME > CREATE LOBBY
Start-Sleep 4
if ($Story) { Press $hostId down; Press $hostId left }           # GAME MODE: back one = STORY CO-OP
Press $guestId rb; Start-Sleep 2; Press $guestId a               # FIND A GAME > the first game
Start-Sleep 3
Press $guestId x; Start-Sleep 2                                  # ready
Press $hostId x                                                  # start

# The lobby copies start the session games; wait until both show the match.
$pidFile = Join-Path $GameDir "FGUYMod\test_pids.txt"
$until = (Get-Date).AddSeconds(150)
$sessions = @()
while ((Get-Date) -lt $until) {
    Start-Sleep 3
    $sessions = @(Get-Content $pidFile | ForEach-Object { [int]$_ } | Where-Object { $_ -gt 0 -and $_ -ne $hostId -and $_ -ne $guestId } | Select-Object -Unique)
    $shown = @($sessions | Where-Object { (Test-Path (Log $_)) -and (Select-String -Path (Log $_) -Pattern "window shown" -SimpleMatch -Quiet) })
    if ($sessions.Count -ge 2 -and $shown.Count -ge 2) { break }
}
Write-Output "session games: $($sessions -join ', ')"
Start-Sleep $Seconds
foreach ($id in @($hostId, $guestId) + $sessions) {
    Write-Output "== $id"
    Select-String -Path (Log $id) -Pattern "Sounds: (online|loading)|Handoff: (the match|window)|DESYNC|SplitView: 2|Online: the session" |
        ForEach-Object { $_.Line.Substring(15) } | Select-Object -First 8
    $sync = @(Select-String -Path (Log $id) -Pattern "in sync with").Count
    if ($sync) { Write-Output "  sync checks logged as in sync: $sync" }
}
