# Three players on one PC (keyboard host, two controller guests): the match runs in sync, each shows its own
# view full screen, then the host's QUIT WITH PARTY brings all three back to the lobby.
#   test_three.ps1        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [int]$Players = 3)
$P = Split-Path $PSScriptRoot -Parent
$shots = Join-Path $P "build\shots"
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) { if (-not (Test-Path (Log $id))) { return 0 }; @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count }
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function WaitFor([int]$id, [string]$text, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (Has $id $text) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$controls = @("keyboard") + @(1..($Players - 1) | ForEach-Object { "controller" })
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Players $Players -Controls ($controls -join ',')
$all = ($out | Where-Object { $_ -match "^all lobby copies: " }) -replace "^all lobby copies: ", ""
$own = @($all -split ', ' | ForEach-Object { [int]$_ })
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $own -notcontains $_ -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) } | Select-Object -Unique)
$bySlot = @{}
foreach ($s in $sess) { for ($k = 0; $k -lt $Players; $k++) { if (Has $s "slot $k of $Players players") { $bySlot[$k] = $s } } }
Step "the match runs with $Players players ($(($bySlot.Values) -join ', '))" ($bySlot.Count -eq $Players)
Start-Sleep 20
for ($k = 0; $k -lt $Players; $k++) {
    $id = $bySlot[$k]
    & "$PSScriptRoot\window_shot.ps1" -Id $id -Out (Join-Path $shots "three_slot$k.png")
    $sync = Count $id "in sync with"
    $bad = Count $id "DESYNC"
    Write-Output "      slot ${k}: $sync sync checks in sync, $bad mismatches; $((Select-String -Path (Log $id) -Pattern 'SplitView: \d' | Select-Object -First 1).Line)"
}
$late = 0
foreach ($id in $bySlot.Values) { $late += @(Select-String -Path (Log $id) -Pattern "DESYNC at tick (\d+)" | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 1200 }).Count }
Step "no mismatch after the level start" ($late -eq 0)
$h = $bySlot[0]
& "$PSScriptRoot\kbm_test.ps1" -Id $h "cursor 300 300" "key 27 150" "wait 800" "key 40 150" "wait 300" "key 40 150" "wait 300" "key 40 150" "wait 300" "key 13 150" | Out-Null
foreach ($o in $own) { Step "lobby game $o back in the lobby" (WaitFor $o "rejoining lobby" 60) }
