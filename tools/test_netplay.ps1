# Netplay determinism test on one PC: two windowed copies of the game boot straight into the same
# level and run in lockstep with scripted inputs, comparing a hash of the players' positions every
# second. Each copy saves to its own scratch save folder. Prints both process ids.
#   test_netplay.ps1 [-Ini boot_frxx.ini] [-Delay 3]
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [string]$Ini = "boot_frxx.ini",
    [int]$Delay = 3,
    [int]$Players = 2
)
& (Join-Path $PSScriptRoot "stop_tests.ps1") -GameDir $GameDir   # only copies tests started
Start-Sleep -Seconds 2
$start = Join-Path $PSScriptRoot "start_test.ps1"
$ids = @()
foreach ($slot in 0..($Players - 1)) {
    $peers = (0..($Players - 1) | Where-Object { $_ -ne $slot } | ForEach-Object { "127.0.0.1:$(7600 + $_)" }) -join ","
    $env:FGUYMOD_NET = "1"; $env:FGUYMOD_NET_SLOT = "$slot"; $env:FGUYMOD_NET_PLAYERS = "$Players"
    $env:FGUYMOD_NET_PORT = "$(7600 + $slot)"; $env:FGUYMOD_NET_PEERS = $peers
    $env:FGUYMOD_NET_DELAY = "$Delay"; $env:FGUYMOD_NET_SCRIPT = "1"; $env:FGUYMOD_SAVEDIR = "testsave$slot"
    $ids += & $start -Ini $Ini -Res "960x540"
    Start-Sleep -Seconds 1
}
foreach ($v in "FGUYMOD_NET", "FGUYMOD_NET_SLOT", "FGUYMOD_NET_PLAYERS", "FGUYMOD_NET_PORT", "FGUYMOD_NET_PEERS",
               "FGUYMOD_NET_DELAY", "FGUYMOD_NET_SCRIPT", "FGUYMOD_SAVEDIR") {
    Set-Item "Env:$v" -Value $null -ErrorAction SilentlyContinue
}
$ids
