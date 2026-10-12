# A keyboard / mouse host actually playing (looking around, walking, shooting) against the bot - the two games
# must stay in sync. -HostRes gives the host's window another size than the bot's (the bot: 640x360).
#   test_kbm_sync.ps1 [-HostRes 1600x900] [-Rate 144] [-Seconds 40]      (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [string]$HostRes = "", [int]$Rate = 0,
      [int]$Seconds = 40)
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }
function Kbm([int]$id, [string]$c) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$id", [System.IO.Pipes.PipeDirection]::Out)
    try { $pipe.Connect(3000); $w = New-Object System.IO.StreamWriter($pipe); $w.WriteLine("kbm $c"); $w.Flush(); $w.Dispose() } catch {}
}
$started = Get-Date
if ($Rate) { $env:FGUYMOD_TEST_RATE = "$Rate" }
$botArgs = @("-Seconds", "3", "-Controls", "keyboard")
if ($HostRes) { $botArgs += @("-HostRes", $HostRes) }
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_bot.ps1" @botArgs
$env:FGUYMOD_TEST_RATE = $null
$logs = Get-ChildItem (Join-Path $GameDir "FGUYMod\FGUYMod_*.log") | Where-Object { $_.LastWriteTime -gt $started } | Sort-Object LastWriteTime -Descending | Select-Object -First 5
$sess = @($logs | Where-Object { Select-String $_ -Pattern "frames per second in the match" -Quiet } | ForEach-Object { [int]($_.BaseName -replace 'FGUYMod_', '') })
$h = $sess | Where-Object { Select-String (Log $_) -Pattern "slot 0 of 2 players" -Quiet } | Select-Object -First 1
$b = $sess | Where-Object { Select-String (Log $_) -Pattern "slot 1 of 2 players" -Quiet } | Select-Object -First 1
Step "the match runs (host $h, bot $b)" ([bool]($h -and $b))
Start-Sleep 15   # past the level start
$until = (Get-Date).AddSeconds($Seconds)
$i = 0
while ((Get-Date) -lt $until) {
    switch ($i % 6) {
        0 { Kbm $h "mouse 25 0 40"; Kbm $h "key 87 700" }      # turn right, walk forward
        1 { Kbm $h "mouse 0 -12 30"; Kbm $h "click 400" }      # look up, shoot
        2 { Kbm $h "mouse -40 6 30"; Kbm $h "key 65 500" }     # turn left, strafe
        3 { Kbm $h "mouse 0 14 30"; Kbm $h "click 600" }       # look down, shoot
        4 { Kbm $h "key 32 150"; Kbm $h "mouse 60 0 20" }      # jump, fast turn
        5 { Kbm $h "key 68 500"; Kbm $h "click 300" }          # strafe, shoot
    }
    $i++
    Start-Sleep -Milliseconds 800
}
Start-Sleep 3
$bad = Select-String (Log $h) -Pattern "out of sync since" -Quiet
$mism = @(Select-String (Log $h) -Pattern "DESYNC at tick (\d+)" | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 1000 })
if ($mism.Count) { Write-Output "      first late mismatch: $($mism[0].Line)"; Select-String (Log $h) -Pattern "^\S+ Netplay:   tick" | Select-Object -First 4 | ForEach-Object { "      host: $($_.Line)" } }
Step "the match stays in sync while the host plays ($($mism.Count) mismatches after tick 1000)" (-not $bad -and $mism.Count -eq 0)
