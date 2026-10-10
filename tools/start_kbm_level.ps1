# Research: starts a test copy in the game's own keyboard / mouse mode (not controller mode) and walks it into
# story level 1 with real key presses (kbm_send.ps1 - it takes the keyboard focus). Prints the process id.
#   start_kbm_level.ps1 [-Res 1280x720]
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [string]$Res = "1280x720"
)
& (Join-Path $PSScriptRoot "stop_tests.ps1") -GameDir $GameDir | Out-Null
$vars = @{ FGUYMOD_TESTINPUT = "1"; FGUYMOD_MODE = "windowed"; FGUYMOD_RES = $Res; FGUYMOD_BACKGROUND = "1"; FGUYMOD_SAVEDIR = "testsave0" }
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
$p = Start-Process -FilePath "$GameDir\FGUY.exe" -WorkingDirectory $GameDir -PassThru
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
Add-Content -Path (Join-Path $GameDir "FGUYMod\test_pids.txt") -Value $p.Id -Encoding ascii
$log = Join-Path $GameDir "FGUYMod\FGUYMod_$($p.Id).log"
function WaitLog([string]$text, [int]$seconds) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern $text -SimpleMatch -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
WaitLog "start_screen" 60 | Out-Null       # the title screen movie is loaded
Start-Sleep 6
$send = Join-Path $PSScriptRoot "kbm_send.ps1"
& $send -Id $p.Id -Steps "click:left:100", "wait:2500", "key:Enter:100", "wait:3000", "click:left:100", "wait:2500", "key:Enter:100", "wait:4000", "key:Enter:100", "wait:3000", "key:Enter:100" | Out-Null
if (-not (WaitLog "HUD split mode" 120)) { Write-Warning "the level didn't start" }
$p.Id
