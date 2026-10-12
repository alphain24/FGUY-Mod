# A player's connection drops out: first for 4 s (everyone waits - "Waiting for Guest..." - then the match goes
# on, in sync), then for good (after 10 s the host's match goes on without them - "Guest left the match" - and
# the host isn't thrown out with them).
#   test_dropout.ps1        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
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
function Pipe([int]$id, [string]$line) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$id", [System.IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(3000)
    $w = New-Object System.IO.StreamWriter($pipe)
    $w.WriteLine($line); $w.Flush(); $w.Dispose()
}

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
$out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Out-Null
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))
Start-Sleep 12   # past the level start
Pipe $c "netstall 4000"
Step "the host waits for the guest, and says so" (WaitFor $h "Notice: Waiting for Guest" 4)
Start-Sleep 7
$before = (Count $h "DESYNC") + (Count $c "DESYNC")
$ticks = Count $h "ticks/s"
Start-Sleep 12
Step "then the match goes on" ((Count $h "ticks/s") -gt $ticks)
Step "in sync" (((Count $h "DESYNC") + (Count $c "DESYNC")) -eq $before -and -not (Has $h "they left"))
Pipe $c "netstall 15000"
Step "a long drop: the host plays on without the guest" (WaitFor $h "they left; their character stays idle" 16)
Step "and says so" (WaitFor $h "Notice: Guest left the match" 3)
Start-Sleep 15
Step "the host isn't thrown out with them" (-not (Has $h "out of sync since") -and (Get-Process -Id $h -ErrorAction SilentlyContinue))
Write-Output "      guest's own game: $((Select-String -Path (Log $c) -Pattern 'out of sync since|Notice:' | Select-Object -Last 2 | ForEach-Object { $_.Line.Substring(15) }) -join ' / ')"
