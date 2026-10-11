# Starts one test copy of the game: windowed, keeps running in the background, virtual pad enabled.
# Prints the process id, and records it so stop_tests.ps1 can close it later (and nothing else).
# Usage: start_test.ps1 [-Ini fguy.ini] [-Res 1280x720]
param(
    [string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse",
    [string]$Ini = "",
    [string]$Res = "1280x720"
)
$env:FGUYMOD_TESTINPUT = "1"; $env:FGUYMOD_MODE = "windowed"; $env:FGUYMOD_RES = $Res
$env:FGUYMOD_BACKGROUND = "1"; $env:FGUYMOD_PADMODE = "1"; $env:FGUYMOD_MUTE = "1"   # no sound from test copies
if ($Ini) { $env:FGUYMOD_INI = $Ini }
$p = Start-Process -FilePath "$GameDir\FGUY.exe" -WorkingDirectory $GameDir -PassThru
Add-Content -Path (Join-Path $GameDir "FGUYMod\test_pids.txt") -Value $p.Id -Encoding ascii
foreach ($v in "FGUYMOD_TESTINPUT", "FGUYMOD_MODE", "FGUYMOD_RES", "FGUYMOD_BACKGROUND", "FGUYMOD_PADMODE", "FGUYMOD_MUTE", "FGUYMOD_INI") {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
$p.Id
