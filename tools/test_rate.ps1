# Frame rate test: a bot match at a given rate (FGUYMOD_TEST_RATE) where the keyboard host also plays - runs,
# turns with the mouse, shoots, jumps - for a while; then whether the games stayed in sync after the level start.
#   test_rate.ps1 -Rate 144 [-Seconds 60]       (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [int]$Rate = 144, [int]$Seconds = 60)
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
$env:FGUYMOD_TEST_RATE = "$Rate"
powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_bot.ps1" -Seconds 3 -Controls keyboard | Out-Null
$env:FGUYMOD_TEST_RATE = $null
$logs = Get-ChildItem (Join-Path $GameDir "FGUYMod\FGUYMod_*.log") | Sort-Object LastWriteTime -Descending | Select-Object -First 4
$sess = @($logs | Where-Object { Select-String $_ -Pattern "frames per second in the match" -Quiet } | ForEach-Object { [int]($_.BaseName -replace 'FGUYMod_', '') })
$h = $sess | Where-Object { Select-String (Log $_) -Pattern "slot 0 of 2 players" -Quiet } | Select-Object -First 1
Write-Output "session games: $($sess -join ', '); host $h"
$until = (Get-Date).AddSeconds($Seconds)
$i = 0
while ((Get-Date) -lt $until) {   # the host plays: forward / strafe, turn, shoot, jump
    $keys = @(87, 65, 83, 68)
    & "$PSScriptRoot\kbm_test.ps1" -Id $h "key $($keys[$i % 4]) 900" "mouse $(15 - 30 * ($i % 2)) $(3 - 6 * ($i % 3 -eq 0)) 40" "click 200" "key 32 100" "wait 300" | Out-Null
    $i++
}
Start-Sleep 3
foreach ($id in $sess) {
    $late = @(Select-String (Log $id) -Pattern "DESYNC at tick ([0-9]+)" | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 1800 })
    $ok = @(Select-String (Log $id) -Pattern "in sync with").Count
    $tps = (Select-String (Log $id) -Pattern "ticks/s" | Select-Object -Last 1).Line
    Write-Output "${id}: $ok in sync, $($late.Count) mismatches after the switch; $($tps.Substring(15, 64))"
    $late | Select-Object -First 3 | ForEach-Object { "   " + $_.Line.Substring(15) }
}
