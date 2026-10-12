# Key bindings online: the keyboard host plays with jump on J instead of Space (FGUYMOD_TEST_KEYCONFIG - the
# copies on one PC share the registry). Its J must go out as the default's Space, and the match stay in sync.
#   test_keybinds.ps1        (needs a lobby server)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$P = Split-Path $PSScriptRoot -Parent
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) { if (-not (Test-Path (Log $id))) { return 0 }; @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count }
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function Step([string]$what, [bool]$ok) { if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 } }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
& "$PSScriptRoot\wait_gpu.ps1"
$env:FGUYMOD_TEST_KEYCONFIG = "8=0x4A"   # jump: J
$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
Remove-Item Env:FGUYMOD_TEST_KEYCONFIG
$out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Out-Null
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c)" ([bool]($h -and $c))
Step "the host's session knows its bindings differ" (Has $h "my key bindings differ from the defaults for 1 actions")
& "$PSScriptRoot\kbm_test.ps1" -Id $h "key 74 300" "wait 1500" "key 74 300" "wait 1500" | Out-Null
Step "J goes out as Space" (Has $h "action 8: my key 0x4a goes out as its default 0x20")
$before = (Count $h "DESYNC") + (Count $c "DESYNC")
Start-Sleep 15
Step "and the match stays in sync" ((Count $h "DESYNC") + (Count $c "DESYNC") -eq $before)
