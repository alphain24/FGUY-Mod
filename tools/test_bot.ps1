# Test bot check on one PC: a test copy goes to ONLINE and hosts a lobby, "Start test bot.bat" starts the bot,
# the bot should join, pick someone and ready up by itself; then the host starts and both play.
#   test_bot.ps1 [-Story] [-Seconds 40]
# Needs a lobby server (python server\fguy_lobby.py). Leaves everything running (stop_tests.ps1 +
# "Stop test bot.bat" close it).
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [switch]$Story,
    [int]$Seconds = 40,
    [string]$Controls = "controller"   # the host copy: controller (virtual pad) / keyboard
)
$pad = Join-Path $PSScriptRoot "pad.ps1"
function Press([int]$id, [string]$button) { & $pad -Id $id $button | Out-Null; Start-Sleep -Milliseconds 600 }
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Has([int]$id, [string]$text) { (Test-Path (Log $id)) -and (Select-String -Path (Log $id) -Pattern $text -SimpleMatch -Quiet) }

$botFile = Join-Path $GameDir "FGUYMod\bot_pids.txt"
Remove-Item $botFile -ErrorAction SilentlyContinue
$ids = & (Join-Path $PSScriptRoot "test_online.ps1") -GameDir $GameDir -Players 1 -Controls $Controls
$hostId = $ids[-1]
Write-Output "host copy $hostId"
Start-Sleep 2
Press $hostId down; Press $hostId down; Press $hostId a          # HOST A GAME > CREATE LOBBY
Start-Sleep 3
if ($Story) { Press $hostId down; Press $hostId left }           # GAME MODE: back one = STORY CO-OP

Start-Process -FilePath (Join-Path $GameDir "FGUYMod\Start test bot.bat") -WindowStyle Hidden
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
