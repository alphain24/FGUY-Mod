# Test tool: first person with every character - each in turn in the test arena (start_arena.ps1: no enemies), a
# contact sheet of their view: third person, first person standing, firing, zoomed in, firing zoomed, reloading.
#   test_fpv_all.ps1 [-Out <folder>] [-From 0] [-To 15]
# -From / -To: characters by their place on the character select (0 Peter, then to the right). The sheets are
# <Out>\char_<n>.png; the research copy is closed after.
param([string]$Out = "$env:TEMP\fpv_all", [int]$From = 0, [int]$To = 15, [string]$GameDir = "A:\Games\FGUY-Research")
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($n in $From..$To) {
    $h = & "$root\tools\start_arena.ps1" -GameDir $GameDir -Right $n -FirstPerson | Select-Object -Last 1
    Start-Sleep 3   # (the gun's hold is taken once they've stood still a moment)
    # ("first" is the gun's guess from the relaxed stance; zoomed in a moment, it's taken from the game's own aim)
    python "$root\tools\re\fpv_frames.py" $h "$Out\char_$n.png" "third=fpv 0 0@1200" "first=fpv 0 1@2500" `
        "aiming=hold 0 1800 lt=255@1600" "standing=-@1500" "fire=hold 0 100 rt=255@120,400" `
        "zoom=hold 0 2500 lt=255@1500" "zfire=hold 0 600 lt=255 rt=255@150" "reload=hold 0 100 x@400,1000" `
        "run=hold 0 1500 ly=32767@900" "stop=-@150,400" "walkreload=hold 0 1800 ly=14000 x@600,1200" `
        "melee=hold 0 100 b@300"
    "character $n -> $Out\char_$n.png"
}
& "$root\tools\stop_tests.ps1" -GameDir $GameDir | Out-Null
