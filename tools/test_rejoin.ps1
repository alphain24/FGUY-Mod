# Rejoin test on one PC: a match where both players move by themselves (the test bot's script), then
#   1. the guest quits to the lobby, picks REJOIN MATCH there, catches up and is back in - in sync;
#   2. the guest's game "crashes" (ends with a crash's exit code): their own game comes back to the lobby, REJOIN MATCH
#      again, back in - in sync.
#   test_rejoin.ps1 [-Story] [-Before 40] [-After 30]     (needs a lobby server: python server\fguy_lobby.py)
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [switch]$Story,
    [int]$Before = 40,    # seconds of match before the guest leaves
    [int]$After = 30,     # seconds of match after they're back
    [switch]$NoCrash,     # only the quit-and-rejoin part
    [switch]$Free         # the new netcode (a free-running match): rejoining loads the level and joins in, no catching up
)
# what the logs say, by netcode
$rejoinsText = if ($Free) { 'rejoining a free-running match' } else { 'rejoins a running match' }
$caughtText = if ($Free) { 'rejoined the match' } else { 'caught up with the match' }
$backText = if ($Free) { 'rejoined the match' } else { 'is back in the match from tick' }
$goneText = if ($Free) { 'they left; their character stays idle' } else { 'they left; their character stays idle' }
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) { if (-not (Test-Path (Log $id))) { return 0 }; @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count }
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function WaitFor([int]$id, [string]$text, [int]$seconds, [int]$times = 1) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if ((Count $id $text) -ge $times) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }
function Pipe([int]$id, [string]$line) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$id", [System.IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(3000)
    $w = New-Object System.IO.StreamWriter($pipe)
    $w.WriteLine($line); $w.Flush(); $w.Dispose()
}
function Pids() { @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ }) }
function Lines([int]$id, [string]$pattern, [int]$last = 3) {
    Select-String -Path (Log $id) -Pattern $pattern | Select-Object -Last $last | ForEach-Object { "      $id " + $_.Line }
}
# The guest's own game is back in the lobby: REJOIN MATCH (X), and their new session game catches up.
function Rejoin([int]$cOwn, [int]$times) {
    Step "the guest's own game is back in the lobby" (WaitFor $cOwn "rejoining lobby" 90 $times)
    Start-Sleep 6
    $before = Pids
    $acts = Count $cOwn "lobby action rejoin"
    & "$PSScriptRoot\pad.ps1" -Id $cOwn x | Out-Null
    Step "REJOIN MATCH picked" (WaitFor $cOwn "lobby action rejoin" 10 ($acts + 1))
    $until = (Get-Date).AddSeconds(30); $new = 0
    while (-not $new -and (Get-Date) -lt $until) {
        Start-Sleep 1
        $new = Pids | Where-Object { $before -notcontains $_ -and (Has $_ $rejoinsText) } | Select-Object -Last 1
    }
    Step "a new session game rejoins ($new)" ([bool]$new)
    $t0 = Get-Date
    $ok = WaitFor $new $caughtText 600
    if (-not $ok) { Lines $new "Netplay: (couldn't|catching up)" 6 }
    Step ("it caught up, in {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds) $ok
    Lines $new "caught up|back in the match from|rejoins the match" 4
    $script:rejoined = $new
}
function SyncAfter([int]$h, [int]$c, [int]$seconds) {
    $d0 = (Count $h "DESYNC") + (Count $c "DESYNC")
    $s0 = Count $c "in sync with peer"
    Start-Sleep $seconds
    $d1 = (Count $h "DESYNC") + (Count $c "DESYNC")
    $s1 = Count $c "in sync with peer"
    Write-Output "      in-sync lines on the rejoined game: $($s1 - $s0); desyncs: $($d1 - $d0)"
    Step "the match goes on in sync with the rejoined player" ($d1 -eq $d0 -and -not (Has $h "out of sync since") -and -not (Has $c "out of sync since"))
    Step "both games still run" ([bool](Get-Process -Id $h -ErrorAction SilentlyContinue) -and [bool](Get-Process -Id $c -ErrorAction SilentlyContinue))
}

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$env:FGUYMOD_NET_SCRIPT = "1"   # (both players move about by themselves - the test bot's script)
if ($Free) { $env:FGUYMOD_TEST_NETFREE = "1" }
$extra = @(); if ($Story) { $extra += "-Story" }   # (a switch through -File: its name only)
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls "controller,controller" @extra   # (the script drives pads)
Remove-Item Env:FGUYMOD_NET_SCRIPT
Remove-Item Env:FGUYMOD_TEST_NETFREE -ErrorAction SilentlyContinue
$out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Out-Null
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Pids | Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c; their own games $hOwn, $cOwn)" ([bool]($h -and $c))
Start-Sleep $Before

# 1. the guest quits to the lobby, and comes back
Pipe $c "netleave"
Step "the guest leaves the match" (WaitFor $h "left the match (back to the lobby)" 20)
Rejoin $cOwn 1; $c2 = $script:rejoined
Step "the host has them back" (WaitFor $h $backText 20)
Lines $h "is rejoining|nearly caught up|is back in the match" 3
SyncAfter $h $c2 $After
if ($NoCrash) { return }

# 2. the guest's game crashes, and they come back
Pipe $c2 "crashexit"
Step "the guest's own game sees it close" (WaitFor $cOwn "closed unexpectedly" 20)
Step "the host plays on without them" (WaitFor $h "they left; their character stays idle" 20)
Rejoin $cOwn 2; $c3 = $script:rejoined
Step "the host has them back again" (WaitFor $h $backText 20 2)
SyncAfter $h $c3 $After
