# The level's start-up must happen the same tick on every PC (loadsync.cpp): a bot match with the host at another
# window size; both match copies' "the players' views start at tick N" must agree, and stay in sync.
#   test_levelstart.ps1 [-HostRes 1600x900] [-Runs 1]      (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse", [string]$HostRes = "1600x900",
      [int]$Runs = 1)
$P = Split-Path $PSScriptRoot -Parent
$bad = 0
for ($r = 1; $r -le $Runs; $r++) {
    & (Join-Path $GameDir "FGUYMod\Stop test bot.bat") | Out-Null
    powershell -NoProfile -File "$PSScriptRoot\stop_tests.ps1" | Out-Null
    $started = Get-Date
    powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_bot.ps1" -Seconds 20 -HostRes $HostRes | Out-Null
    $sess = @(Get-ChildItem (Join-Path $GameDir "FGUYMod\FGUYMod_*.log") | Where-Object { $_.LastWriteTime -gt $started } |
        Where-Object { Select-String $_ -Pattern "frames per second in the match" -Quiet })
    $ticks = @()
    foreach ($f in $sess) {
        $v = Select-String $f -Pattern "the players' views start at tick (\d+)" | Select-Object -First 1
        $w = (Select-String $f -Pattern "Online: tick \d+ - waited" | ForEach-Object { $_.Line -replace '^\S+ ', '' }) -join '; '
        $late = @(Select-String $f -Pattern "DESYNC at tick (\d+)" | Where-Object { [int]$_.Matches[0].Groups[1].Value -gt 1000 }).Count
        $t = if ($v) { [int]$v.Matches[0].Groups[1].Value } else { -1 }
        $ticks += $t
        Write-Output "  run $r $($f.BaseName): views start at tick $t, $late mismatches after tick 1000; $w"
    }
    $same = $ticks.Count -eq 2 -and $ticks[0] -ge 0 -and $ticks[0] -eq $ticks[1]
    if (-not $same) { $bad++ }
    Write-Output ("run ${r}: " + $(if ($same) { "ok - the same tick" } else { "DIFFERENT" }))
}
& (Join-Path $GameDir "FGUYMod\Stop test bot.bat") | Out-Null
powershell -NoProfile -File "$PSScriptRoot\stop_tests.ps1" | Out-Null
if ($bad) { Write-Output "FAIL: $bad of $Runs runs started the level at different ticks"; exit 1 }
Write-Output "ok:   every run started the level the same tick on both PCs"
