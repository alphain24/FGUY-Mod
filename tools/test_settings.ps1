# Online pause SETTINGS test on one PC: a match (keyboard host, controller guest). Each player's own settings
# reach their session game; the host opens SETTINGS from the online pause menu, raises the music and the aim
# sensitivity (the match must stay in sync), the guest opens it too; then QUIT WITH PARTY - the host's own game
# gets the changed settings back.
#   test_settings.ps1        (needs a lobby server: python server\fguy_lobby.py)
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
function Keys([int]$id, [string[]]$steps) { & "$PSScriptRoot\kbm_test.ps1" -Id $id @steps | Out-Null }
function Pad([int]$id, [string[]]$buttons) { & "$PSScriptRoot\pad.ps1" -Id $id @buttons | Out-Null }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
$line = $out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Select-Object -First 1
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))
Step "the lobby games read their own settings" ((Has $hOwn "Settings: my settings aim=") -and (Has $cOwn "Settings: my settings aim="))
Step "the session games got them" ((Has $h "Settings: mine for this match") -and (Has $c "Settings: mine for this match"))
Step "and applied the sound over the host's save" ((Has $h "my volumes and subtitles applied") -and (Has $c "my volumes and subtitles applied"))

# host: Esc, down (SETTINGS), Enter -> the page; right on Music; down x7 to Aim sensitivity, right twice
Keys $h @("key 27 150", "wait 800", "key 40 150", "wait 400", "key 13 150", "wait 800")
Step "the host's settings page opens" (WaitFor $h "Pause: settings" 5)
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "settings_host.png")
Keys $h @("key 39 150", "wait 400")
for ($i = 0; $i -lt 6; $i++) { Keys $h @("key 40 120", "wait 250") }
Keys $h @("key 37 150", "wait 400", "key 37 150", "wait 600")   # aim: two steps down
Keys $h @("key 40 150", "wait 300", "key 13 150", "wait 600", "key 38 150", "wait 300")   # invert look on
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "settings_host_aim.png")
Keys $h @("cursor 300 300", "wait 200")
for ($i = 0; $i -lt 12; $i++) { Keys $h @("key 40 120", "wait 250") }   # to the bottom (SCREEN, BACK)
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "settings_host_bottom.png")
Keys $h @("key 27 150", "wait 400", "key 27 150", "wait 600")   # back, close (the guest's turn: one menu open)
# guest: Start, down, A -> the page; B, B
Pad $c @("start"); Start-Sleep 1; Pad $c @("down", "a"); Start-Sleep 1
Step "the guest's settings page opens" (WaitFor $c "Pause: settings" 5)
& "$PSScriptRoot\window_shot.ps1" -Id $c -Out (Join-Path $shots "settings_guest.png")
Pad $c @("b", "b")
$aimH = (python (Join-Path $P "build\peekf.py") $h bad810 bad814) -join " "
$aimC = (python (Join-Path $P "build\peekf.py") $c bad810 bad814) -join " "
Write-Output "      aim sensitivity per player - host's PC: $aimH"
Write-Output "      aim sensitivity per player - guest's PC: $aimC"
Step "both PCs have the same aim settings, with the host's lowered" (($aimH -eq $aimC) -and ($aimH -match "f=0.9"))
$invH = (python (Join-Path $P "build\peekf.py") $h b5e590 b62200) -join " "
$invC = (python (Join-Path $P "build\peekf.py") $c b5e590 b62200) -join " "
Write-Output "      invert look per view - host's PC: $invH"
Write-Output "      invert look per view - guest's PC: $invC"
Step "both PCs have the same invert settings" ($invH -eq $invC)
$desyncBefore = (Count $h "DESYNC") + (Count $c "DESYNC")
Start-Sleep 25
$desyncAfter = (Count $h "DESYNC") + (Count $c "DESYNC")
Step "the match stays in sync after the change ($desyncBefore before)" ($desyncAfter -eq $desyncBefore)
Step "and keeps checking" ((Count $h "in sync with") -ge 3)

# QUIT WITH PARTY: Esc, down x3, Enter
Keys $h @("key 27 150", "wait 800", "key 40 150", "wait 300", "key 40 150", "wait 300", "key 40 150", "wait 300", "key 13 150")
Step "host's own game back in the lobby" (WaitFor $hOwn "rejoining lobby" 60)
Step "with the settings from the match" (WaitFor $hOwn "Settings: back from the match with aim=90;inv=1" 15)
Select-String -Path (Log $hOwn) -Pattern "Settings: (my settings|back from)" | ForEach-Object { "      " + $_.Line.Substring(15) }
