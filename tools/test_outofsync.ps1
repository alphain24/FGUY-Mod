# Out of sync, the way it happens: a bot match with a keyboard host (no controller plugged in); the host's
# player is nudged on its PC only (pipe "nudge"). Both games must say so and come back to the lobby - the host's
# match copy must not hang on "reconnect the controller" on its way out.
#   test_outofsync.ps1 [-Rate 144]        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [int]$Rate = 0)
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Has([int]$id, [string]$text) { (Test-Path (Log $id)) -and (Select-String -Path (Log $id) -Pattern $text -SimpleMatch -Quiet) }
function WaitFor([int]$id, [string]$text, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if (Has $id $text) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }
$started = Get-Date
if ($Rate) { $env:FGUYMOD_TEST_RATE = "$Rate" }
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_bot.ps1" -Seconds 3 -Controls keyboard -HostRes 1600x900
$env:FGUYMOD_TEST_RATE = $null
$hostLobby = [int](($out | Where-Object { $_ -match "^host copy (\d+)" } | Select-Object -First 1) -replace "^host copy ", "")
$logs = Get-ChildItem (Join-Path $GameDir "FGUYMod\FGUYMod_*.log") | Where-Object { $_.LastWriteTime -gt $started } | Sort-Object LastWriteTime -Descending | Select-Object -First 5
$sess = @($logs | Where-Object { Select-String $_ -Pattern "frames per second in the match" -Quiet } | ForEach-Object { [int]($_.BaseName -replace 'FGUYMod_', '') })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -First 1
$b = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -First 1
Step "the match runs (host $h, bot $b; host's own game $hostLobby)" ([bool]($h -and $b -and $hostLobby))
Start-Sleep 15   # past the level start (mismatches there don't count)
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$h", [System.IO.Pipes.PipeDirection]::Out)
$pipe.Connect(3000); $w = New-Object System.IO.StreamWriter($pipe); $w.WriteLine("nudge"); $w.Flush(); $w.Dispose()
Step "the host's game sees it go out of sync" (WaitFor $h "out of sync since" 10)
Step "and so does the bot's" (WaitFor $b "out of sync since" 5)
Step "the host's own game is back in the lobby" (WaitFor $hostLobby "the match is over - back to its lobby" 30)
$until = (Get-Date).AddSeconds(20); while ((Get-Process -Id $h -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep 1 }
Step "the host's match copy closed (not stuck)" (-not (Get-Process -Id $h -ErrorAction SilentlyContinue))
