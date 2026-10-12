# Back-to-the-lobby test on one PC: a match (keyboard host, controller guest); the host opens the online pause
# menu and picks QUIT WITH PARTY. Each player's own game (the lobby copy, waiting hidden during the match)
# should come back where the match was, in the same lobby, and the session games close; then the guest readies
# up on the controller, the host starts a second match on the keyboard, and it shows.
#   test_return.ps1        (needs a lobby server: python server\fguy_lobby.py)
param([string]$GameDir = "A:\Games\Activision\Family Guy Back to the Multiverse")
$P = Split-Path $PSScriptRoot -Parent
$shots = Join-Path $P "build\shots"
function Log([int]$id) { Join-Path $GameDir "FGUYMod\FGUYMod_$id.log" }
function Count([int]$id, [string]$text) {
    if (-not (Test-Path (Log $id))) { return 0 }
    @(Select-String -Path (Log $id) -Pattern $text -SimpleMatch).Count
}
function Has([int]$id, [string]$text) { (Count $id $text) -gt 0 }
function WaitFor([int]$id, [string]$text, [int]$seconds, [int]$times = 1) {
    $until = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $until) { if ((Count $id $text) -ge $times) { return $true }; Start-Sleep 1 }
    return $false
}
function Step([string]$what, [bool]$ok) {
    if ($ok) { Write-Output "ok:   $what" } else { Write-Output "FAIL: $what"; exit 1 }
}
function Pipe([int]$id, [string]$line) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "FGUYMod_$id", [System.IO.Pipes.PipeDirection]::Out)
    $pipe.Connect(3000)
    $w = New-Object System.IO.StreamWriter($pipe)
    $w.WriteLine($line); $w.Flush(); $w.Dispose()
}
function Cpu([int]$id) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p) { $p.CPU } else { -1 } }

& "$PSScriptRoot\stop_tests.ps1" -GameDir $GameDir
$until = (Get-Date).AddSeconds(60); while ((Get-Process FGUY -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep 1 }
& "$PSScriptRoot\wait_gpu.ps1"

$out = powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\test_online_match.ps1" -GameDir $GameDir -Seconds 2 -Controls keyboard,controller
$line = $out | Where-Object { $_ -match "^lobby copies: host (\d+), guest (\d+)" } | Select-Object -First 1
$hOwn = [int]$Matches[1]; $cOwn = [int]$Matches[2]
$sess = @(Get-Content (Join-Path $GameDir "FGUYMod\test_pids.txt") | Where-Object { $_ } | ForEach-Object { [int]$_ } |
          Where-Object { $_ -ne $hOwn -and $_ -ne $cOwn -and (Get-Process -Id $_ -ErrorAction SilentlyContinue) })
$h = $sess | Where-Object { Has $_ "slot 0 of 2 players" } | Select-Object -Last 1
$c = $sess | Where-Object { Has $_ "slot 1 of 2 players" } | Select-Object -Last 1
Step "the match runs (host $h, guest $c; their own games $hOwn, $cOwn)" ([bool]($h -and $c))
Step "the players' own games wait hidden" ((Has $hOwn "this game waits hidden") -and (Has $cOwn "this game waits hidden") -and
                                           (Get-Process -Id $hOwn, $cOwn -ErrorAction SilentlyContinue).Count -eq 2)
$cpu0 = (Cpu $hOwn) + (Cpu $cOwn); Start-Sleep 5; $cpu1 = (Cpu $hOwn) + (Cpu $cOwn)
Write-Output ("      waiting games' CPU over 5 s: {0:N2} s" -f ($cpu1 - $cpu0))

# the online pause menu: Esc opens it (the match goes on), down x3 = QUIT WITH PARTY (host only), Enter
& "$PSScriptRoot\kbm_test.ps1" -Id $h "key 27 150" "wait 1200" | Out-Null
& "$PSScriptRoot\window_shot.ps1" -Id $h -Out (Join-Path $shots "pause_menu.png")
Step "the online pause menu opens" (WaitFor $h "Pause: menu open" 5)
& "$PSScriptRoot\kbm_test.ps1" -Id $h "key 40 150" "wait 400" "key 40 150" "wait 400" "key 40 150" "wait 400" "key 13 150" | Out-Null
Step "host's own game back in the lobby" (WaitFor $hOwn "rejoining lobby" 60)
Step "guest's own game back in the lobby" (WaitFor $cOwn "rejoining lobby" 30)
Step "both show again" ((WaitFor $hOwn "Window: shown again" 5) -and (WaitFor $cOwn "Window: shown again" 5))
Step "the lobby's 3D character comes back" ((WaitFor $hOwn "Camera_StoreEnter_ref" 5 2) -and (WaitFor $cOwn "Camera_StoreEnter_ref" 5 2))
$until = (Get-Date).AddSeconds(25); while ((Get-Process -Id $h, $c -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep 1 }
Step "the session games closed" (-not (Get-Process -Id $h, $c -ErrorAction SilentlyContinue))
Start-Sleep 4
& "$PSScriptRoot\window_shot.ps1" -Id $cOwn -Out (Join-Path $shots "returned_lobby.png")
Start-Sleep 20   # the returned lobby must keep drawing (a frame-rate line every 10 s)
foreach ($id in $hOwn, $cOwn) {
    $fps = Select-String -Path (Log $id) -Pattern "D3D: .* fps" | Select-Object -Last 1
    Write-Output "      $id last frame-rate line: $($fps.Line)"
}
$readyBefore = Count $cOwn "lobby action ready"
& "$PSScriptRoot\pad.ps1" -Id $cOwn x | Out-Null
Step "guest readies up on the controller" (WaitFor $cOwn "lobby action ready" 10 ($readyBefore + 1))
$startBefore = Count $hOwn "lobby action start"
& "$PSScriptRoot\pad.ps1" -Id $hOwn x | Out-Null   # (as for the first match: test copies' menus take pads)
Step "host starts the second match" (WaitFor $hOwn "lobby action start" 10 ($startBefore + 1))
Step "the second match shows" (WaitFor $hOwn "the session game is showing the match" 150 2)
