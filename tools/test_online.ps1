# Online test on one PC: starts copies of the game (each with its own copy of a save and its own player
# name), walks each one to Main Menu > ONLINE, and prints their process ids. Needs a lobby server:
#   python server\fguy_lobby.py            (local)   or set -Server to your VPS address.
#   test_online.ps1 [-Players 2] [-Server 127.0.0.1:7650] [-Save <file.sav>]
# The copies skip the intro movies and walk the title, save and main menu by themselves
# (FGUYMOD_TEST_AUTOMENU=online, the online session autopilot's boot steps).
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [int]$Players = 2,
    [string]$Server = "127.0.0.1:7650",
    [string]$Save = (Join-Path $PSScriptRoot "..\backups\Save\player0_default.sav"),
    [string[]]$Controls = @(),  # per copy: keyboard / controller (default: the mod's auto choice)
    [string]$Res = "960x540"     # the copies' render size
)
$Controls = @($Controls | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # ("a,b" from -File comes as one)
& (Join-Path $PSScriptRoot "stop_tests.ps1") -GameDir $GameDir
$start = Join-Path $PSScriptRoot "start_test.ps1"
$names = "Host", "Guest", "Third", "Fourth"
$ids = @()
foreach ($i in 0..($Players - 1)) {
    $dir = Join-Path $GameDir "FGUYMod\testsave$i"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item $Save $dir -Force
    $env:FGUYMOD_SAVEDIR = "testsave$i"; $env:FGUYMOD_ONLINE_NAME = $names[$i]; $env:FGUYMOD_ONLINE_SERVER = $Server
    $env:FGUYMOD_TEST_AUTOMENU = "online"
    if ($Controls.Count -gt $i) { $env:FGUYMOD_ONLINE_CONTROLS = $Controls[$i] } else { Remove-Item Env:FGUYMOD_ONLINE_CONTROLS -ErrorAction SilentlyContinue }
    $ids += & $start -Res $Res
    Start-Sleep -Seconds 1
}
foreach ($v in "FGUYMOD_SAVEDIR", "FGUYMOD_ONLINE_NAME", "FGUYMOD_ONLINE_SERVER", "FGUYMOD_TEST_AUTOMENU", "FGUYMOD_ONLINE_CONTROLS") { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }

function WaitLog([int]$id, [string]$text, [int]$seconds) {
    $log = Join-Path $GameDir "FGUYMod\FGUYMod_$id.log"
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern $text -SimpleMatch -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
foreach ($id in $ids) {   # each copy walks to ONLINE by itself (FGUYMOD_TEST_AUTOMENU)
    if (-not (WaitLog $id "FGUYMOD_OnlineEnter" 120)) { Write-Warning "copy $id didn't reach ONLINE" }
}
$ids
