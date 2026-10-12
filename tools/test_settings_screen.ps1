# The SETTINGS page's SCREEN rows mid-match: the host switches the display mode windowed -> borderless ->
# windowed; the match must keep drawing and stay in sync.
#   test_settings_screen.ps1        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$P = Split-Path $PSScriptRoot -Parent
$shots = Join-Path $P "build\shots"
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) { if (-not (Test-Path (Log $id))) { return 0 }; @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count }
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }
function Keys([int]$id, [string[]]$steps) { & "$PSScriptRoot\kbm_test.ps1" -Id $id @steps | Out-Null }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
$out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Out-Null
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))
Keys $h @("cursor 300 300", "wait 200")   # (the test's pointer in a corner: hover mustn't pick rows)
Keys $h @("key 27 150", "wait 800", "key 40 150", "wait 400", "key 13 150", "wait 800")
for ($i = 0; $i -lt 10; $i++) { Keys $h @("key 40 120", "wait 250") }   # Display mode
Keys $h @("key 39 150", "wait 4000")                                      # borderless
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "settings_borderless.png")
Keys $h @("key 39 150", "wait 4000")                                      # and windowed again
Step "the display mode switches there and back" ((Count $h "Display: switching") -ge 2)
Keys $h @("key 27 150", "wait 400", "key 27 150", "wait 400")
$before = (Count $h "DESYNC") + (Count $c "DESYNC")
$fpsBefore = Count $h "D3D: "
Start-Sleep 25
Step "the host's game keeps drawing" ((Count $h "D3D: ") -gt $fpsBefore)
Step "and the match stays in sync" ((Count $h "DESYNC") + (Count $c "DESYNC") -eq $before -and (Count $h "in sync with") -ge 3)
Select-String -Path (Log $h) -Pattern "Display: switching|Settings: resolution|D3D: Reset|D3D: .*fps" | Select-Object -Last 6 | ForEach-Object { "      " + $_.Line.Substring(15) }
