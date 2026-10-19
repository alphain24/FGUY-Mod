# Test bot check on one PC: a test copy goes to ONLINE and hosts a lobby, "Start test bot.bat" starts the bot,
# the bot should join, pick someone and ready up by itself; then the host starts and both play.
#   test_bot.ps1 [-Story] [-Seconds 40] [-MapLefts 2] [-ModeRights 1]
# Needs a lobby server (python server\fguy_lobby.py). Leaves everything running (stop_tests.ps1 +
# "Stop test bot.bat" close it).
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [switch]$Story,
    [int]$Seconds = 40,
    [string]$Controls = "controller",  # the host copy: controller (virtual pad) / keyboard
    [string]$HostRes = "960x540",
    [int]$MapLefts = 0,                # MAP: this many steps left (2 = AIRPORT'12)
    [int]$ModeRights = 0,              # GAME MODE: this many steps right (1 = MULTIVERSE MADNESS)
    [switch]$NewNet,                   # MATCH SETTINGS > NETCODE: New (test) - the free-running match
    [switch]$FriendlyFire,             # MATCH SETTINGS > FRIENDLY FIRE: on (the bot's team is random)
    [int]$BotTeam = -1                 # the bot's team: 0 Good, 1 Evil (the host's stays Good); -1 at random
)
$pad = Join-Path $PSScriptRoot "pad.ps1"
function Press([int]$id, [string]$button) { & $pad -Id $id $button | Out-Null; Start-Sleep -Milliseconds 600 }
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Has([int]$id, [string]$text) { (Test-Path (Log $id)) -and (Select-String -Path (Log $id) -Pattern $text -SimpleMatch -Quiet) }

$botFile = Join-Path $GameDir "FGUYMod\bot_pids.txt"
# bots from earlier runs (their lobby games come back after a match and would join this one too): closed first
& (Join-Path $GameDir "FGUYMod\Stop test bot.bat") | Out-Null
Start-Sleep 3
$ids = & (Join-Path $PSScriptRoot "test_online.ps1") -GameDir $GameDir -Players 1 -Controls $Controls -Res $HostRes
$hostId = $ids[-1]
Write-Output "host copy $hostId"
Start-Sleep 2
Press $hostId down; Press $hostId down; Press $hostId a          # HOST A GAME > CREATE LOBBY
Start-Sleep 3
if ($Story) { Press $hostId down; Press $hostId left }           # GAME MODE: back one = STORY CO-OP
elseif ($ModeRights) { Press $hostId down; foreach ($k in 1..$ModeRights) { Press $hostId right } }
elseif ($MapLefts) { Press $hostId down; Press $hostId down; foreach ($k in 1..$MapLefts) { Press $hostId left } }
if ($NewNet -or $FriendlyFire) {   # (Deathmatch's rows: mode, map, time, score, friendly fire, frame rate, netcode;
    #  Multiverse Madness has no score row)
    $ffRow = if ($ModeRights -eq 1) { 3 } else { 4 }
    $netRow = if ($ModeRights -eq 1) { 5 } else { 6 }
    $at = if ($Story -or $ModeRights) { 1 } elseif ($MapLefts) { 2 } else { 0 }   # (the row picked above: START, GAME MODE, MAP)
    foreach ($k in ($at + 1)..3) { Press $hostId down }; Press $hostId a   # MATCH SETTINGS
    $row = 0
    if ($FriendlyFire) { foreach ($k in 1..$ffRow) { Press $hostId down }; Press $hostId right; $row = $ffRow }
    if ($NewNet) { foreach ($k in ($row + 1)..$netRow) { Press $hostId down }; Press $hostId right }
    Press $hostId b
}

if ($BotTeam -ge 0) { $env:FGUYMOD_BOT_TEAM = "$BotTeam" }
Start-Process -FilePath (Join-Path $GameDir "FGUYMod\Start test bot.bat") -WindowStyle Hidden
Remove-Item Env:FGUYMOD_BOT_TEAM -ErrorAction SilentlyContinue
$until = (Get-Date).AddSeconds(150)
$botId = 0
while ((Get-Date) -lt $until) {   # the bot readies up: the host's lobby shows it ready
    if (-not $botId -and (Test-Path $botFile)) { $botId = [int](Get-Content $botFile | Select-Object -First 1) }
    if ($botId -and (Has $botId "lobby action ready")) { break }
    Start-Sleep 1
}
Write-Output "bot copy $botId"
if (-not $botId -or -not (Has $botId "lobby action ready")) { Write-Warning "the bot didn't ready up"; return }
Start-Sleep 2
Press $hostId x                                                  # start

$pidFile = Join-Path $GameDir "FGUYMod\test_pids.txt"
$until = (Get-Date).AddSeconds(150)
while ((Get-Date) -lt $until) {
    $sessions = @(Get-Content $pidFile, $botFile | ForEach-Object { [int]$_ } | Where-Object { $_ -gt 0 -and $_ -ne $hostId -and $_ -ne $botId } | Select-Object -Unique)
    if ($sessions.Count -ge 2 -and @($sessions | Where-Object { Has $_ "everyone's level is up" }).Count -ge 2) { break }
    Start-Sleep 1
}
Write-Output "session copies: $($sessions -join ', ')"
Start-Sleep $Seconds
foreach ($id in $sessions) {
    Write-Output "--- $id"
    Select-String -Path (Log $id) -Pattern "Handoff:|everyone's level|test bot|in sync|DESYNC|desync" -SimpleMatch:$false |
        Select-Object -Last 12 | ForEach-Object { $_.Line }
}
