# From the intro trailer / title screen of a test copy, get to the Main Menu with the virtual pad:
# A (leave trailer) -> Start (title) -> A (load the first save slot).
# Don't press A during the very first logo movies: skipping those crashes the game (XAudio2 bug).
$pad = Join-Path $PSScriptRoot "pad.ps1"
function Wait([int]$s) { $i = 0; while ($i -lt $s) { Start-Sleep -Seconds 1; $i++ } }
& $pad a | Out-Null;     Wait 3
& $pad start | Out-Null; Wait 4
& $pad a | Out-Null;     Wait 5
"done"
