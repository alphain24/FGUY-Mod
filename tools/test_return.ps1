# Back-to-the-lobby test on one PC: a match (keyboard host, controller guest), the host pauses and picks
# Quit Game -> Yes; both games should come back to the same lobby; then the guest readies up and the host
# starts a second match from it.
#   test_return.ps1        (needs a lobby server: python server\fguy_lobby.py)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Has([int]$id, [string]$text) { (Test-Path (Log $id)) -and (Select-String -Path (Log $id) -Pattern $text -SimpleMatch -Quiet) }
function WaitFor([int]$id, [string]$text, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (Has $id $text) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) {
    if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 }
}

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
$until = (Get-Date).AddSeconds(60); while ((Get-Process FGUY -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep 1 }
& "$PSScriptRoot\wait_gpu.ps1"

powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller | Out-Null
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))

& "$PSScriptRoot\kbm_test.ps1" -Id $h "key 27 150" "wait 1500" "key 40 150" "wait 500" "key 40 150" "wait 500" "key 40 150" "wait 500" "key 13 150" "wait 1500" "key 37 150" "wait 600" "key 13 150" | Out-Null
Step "host back in the lobby" (WaitFor $h "rejoining lobby" 60)
Step "guest back in the lobby" (WaitFor $c "rejoining lobby" 30)
Start-Sleep 6
& "$PSScriptRoot\pad.ps1" -Id $c x | Out-Null
Step "guest readies up in the lobby" (WaitFor $c "lobby action ready" 10)
& "$PSScriptRoot\pad.ps1" -Id $h x | Out-Null
Step "host starts the second match" (WaitFor $h "lobby action start" 10)
Step "the second match shows" (WaitFor $h "session game is showing" 120)
